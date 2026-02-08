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
#include <functional>
#include <numeric>
#include <limits>
#include <type_traits>
#include <atomic>
#include <cstring>
#include "zlib.h"
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
                currentPos = packet.pos;
                if (!onFrame(frame())) {
                    av_packet_unref(&packet);
                    return;
                }
            }
        }
        av_packet_unref(&packet);
    }

    // flush decoder
    if (avcodec_send_packet(codecCtx(), NULL) != 0) {
        THROW(FormatException, "avcodec_send_packet failed");
    }
    while (avcodec_receive_frame(codecCtx(), frame()) == 0) {
        onFrame(frame());
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
        int count;
        int totalCandidates;
        int rejectedExtreme;
        AutoDetectStats() : sumF(0), sumB(0), sumF2(0), sumB2(0), sumFB(0), count(0), totalCandidates(0), rejectedExtreme(0) {}
    };

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
        if (s.count < 8) {
            return false;
        }

        double A1, A2;
        double B1, B2;
        logo::approxim_line(s.count, s.sumF, s.sumB, s.sumF2, s.sumFB, A1, B1);
        logo::approxim_line(s.count, s.sumB, s.sumF, s.sumB2, s.sumFB, A2, B2);
        if (std::abs(A2) < 1e-12) {
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
        int readFrames;

        std::vector<uint8_t> frameWork8;
        std::vector<uint16_t> frameWork16;
        std::vector<AutoDetectStats> stats;
        std::vector<float> lastSampleFg;
        std::vector<float> lastSampleBg;
        std::vector<uint8_t> lastSampleValid;
        std::vector<float> score;
        std::vector<uint8_t> binary;
        std::vector<float> mapA;
        std::vector<float> mapB;
        std::vector<float> mapAlpha;
        std::vector<float> mapLogoY;
        std::vector<float> mapConsistency;
        std::vector<float> mapBgVar;
        std::vector<float> mapAccepted;
        std::vector<uint8_t> validAB;
        std::vector<int> frameValidCounts;
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

    public:
        AutoDetectLogoReader(AMTContext& ctx, int serviceid, int divx, int divy, int searchFrames, int blockSize, int threshold, int marginX, int marginY, int threadN, logo::LOGO_AUTODETECT_CB cb)
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
            , cb(cb)
            , threadPool(std::max(1, threadN))
            , imgw(0), imgh(0), scanx(0), scany(0), scanw(0), scanh(0), radius(0), bitDepth(8), readFrames(0), frameWork8(), frameWork16(), stats(), lastSampleFg(), lastSampleBg(), lastSampleValid(), score(), binary(), mapA(), mapB(), mapAlpha(), mapLogoY(), mapConsistency(), mapBgVar(), mapAccepted(), validAB(), frameValidCounts(), iterBinaryHistory(), iterThresholdDebug(), promoteCompDebug(), deltaCompDebug(), rectMergeDebug(), debugAbsX(1380), debugAbsY(67), rectAbs{ 0, 0, 0, 0 }, rectLocal{ 0, 0, 0, 0 } {
        }

        AutoDetectRect run(const tstring& srcpath) {
            if (cb && cb(1, 0.0f, 0.0f, 0, searchFrames) == false) {
                THROW(RuntimeException, "Cancel requested");
            }
            readAll(srcpath, serviceid);
            if (readFrames <= 0 || scanw <= 0 || scanh <= 0) {
                THROW(RuntimeException, "No frame decoded");
            }
            estimateScoreAndRect();
            return rectAbs;
        }

        void writeDebug(const tstring& scorePath, const tstring& binaryPath, const tstring& cclPath, const tstring& countPath, const tstring& aPath, const tstring& bPath, const tstring& alphaPath, const tstring& logoYPath, const tstring& consistencyPath, const tstring& bgVarPath, const tstring& acceptedPath) {
            if (scanw <= 0 || scanh <= 0) {
                return;
            }

            const std::string scorePathA = tchar_to_string(scorePath);
            const std::string binaryPathA = tchar_to_string(binaryPath);
            const std::string cclPathA = tchar_to_string(cclPath);
            const std::string countPathA = tchar_to_string(countPath);
            const std::string aPathA = tchar_to_string(aPath);
            const std::string bPathA = tchar_to_string(bPath);
            const std::string alphaPathA = tchar_to_string(alphaPath);
            const std::string logoYPathA = tchar_to_string(logoYPath);
            const std::string consistencyPathA = tchar_to_string(consistencyPath);
            const std::string bgVarPathA = tchar_to_string(bgVarPath);
            const std::string acceptedPathA = tchar_to_string(acceptedPath);

            float maxScore = 0.0f;
            for (auto v : score) maxScore = std::max(maxScore, v);
            if (maxScore <= 0) maxScore = 1.0f;

            WriteGrayBitmap(scorePathA, scanw, scanh, [&](int x, int y) {
                const int off = x + y * scanw;
                const float v = (off < (int)score.size()) ? score[off] : 0.0f;
                return (uint8_t)ClampInt((int)std::round(v / maxScore * 255.0f), 0, 255);
            });
            WriteGrayBitmap(binaryPathA, scanw, scanh, [&](int x, int y) {
                const int off = x + y * scanw;
                return (off < (int)binary.size() && binary[off]) ? 255 : 0;
            });
            WriteGrayBitmap(cclPathA, scanw, scanh, [&](int x, int y) {
                const int off = x + y * scanw;
                const bool hasRect = rectLocal.w > 0 && rectLocal.h > 0;
                const bool onBorder = hasRect && (x == rectLocal.x || x == rectLocal.x + rectLocal.w - 1 || y == rectLocal.y || y == rectLocal.h + rectLocal.y - 1);
                if (onBorder) return (uint8_t)255;
                return (off < (int)binary.size() && binary[off]) ? (uint8_t)190 : (uint8_t)24;
            });

            int maxCount = 0;
            for (const auto& s : stats) {
                maxCount = std::max(maxCount, s.count);
            }
            if (maxCount <= 0) maxCount = 1;
            WriteGrayBitmap(countPathA, scanw, scanh, [&](int x, int y) {
                const int c = stats[x + y * scanw].count;
                return (uint8_t)ClampInt((int)std::round((double)c * 255.0 / maxCount), 0, 255);
            });

            float aMin, aMax, bMin, bMax;
            CalcRangeValid(mapA, validAB, aMin, aMax, 0.8f, 1.2f);
            CalcRangeValid(mapB, validAB, bMin, bMax, -0.2f, 0.2f);
            WriteGrayBitmap(aPathA, scanw, scanh, [&](int x, int y) {
                const int off = x + y * scanw;
                if (off >= (int)validAB.size() || !validAB[off] || off >= (int)mapA.size()) {
                    return (uint8_t)0;
                }
                const float t = (mapA[off] - aMin) / (aMax - aMin);
                return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
            });
            WriteGrayBitmap(bPathA, scanw, scanh, [&](int x, int y) {
                const int off = x + y * scanw;
                if (off >= (int)validAB.size() || !validAB[off] || off >= (int)mapB.size()) {
                    return (uint8_t)0;
                }
                const float t = (mapB[off] - bMin) / (bMax - bMin);
                return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
            });
            WriteGrayBitmap(alphaPathA, scanw, scanh, [&](int x, int y) {
                const int off = x + y * scanw;
                if (off >= (int)validAB.size() || !validAB[off] || off >= (int)mapAlpha.size()) {
                    return (uint8_t)0;
                }
                return (uint8_t)ClampInt((int)std::round(mapAlpha[off] * 255.0f), 0, 255);
            });
            WriteGrayBitmap(logoYPathA, scanw, scanh, [&](int x, int y) {
                const int off = x + y * scanw;
                if (off >= (int)validAB.size() || !validAB[off] || off >= (int)mapLogoY.size()) {
                    return (uint8_t)0;
                }
                return (uint8_t)ClampInt((int)std::round(mapLogoY[off] * 255.0f), 0, 255);
            });
            float csMin, csMax, bgvMin, bgvMax;
            CalcRangeValid(mapConsistency, validAB, csMin, csMax, 0.0f, 2.0f);
            CalcRangeValid(mapBgVar, validAB, bgvMin, bgvMax, 0.0f, 0.02f);
            WriteGrayBitmap(consistencyPathA, scanw, scanh, [&](int x, int y) {
                const int off = x + y * scanw;
                if (off >= (int)validAB.size() || !validAB[off] || off >= (int)mapConsistency.size()) {
                    return (uint8_t)0;
                }
                const float t = (mapConsistency[off] - csMin) / (csMax - csMin);
                return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
            });
            WriteGrayBitmap(bgVarPathA, scanw, scanh, [&](int x, int y) {
                const int off = x + y * scanw;
                if (off >= (int)validAB.size() || !validAB[off] || off >= (int)mapBgVar.size()) {
                    return (uint8_t)0;
                }
                const float t = (mapBgVar[off] - bgvMin) / (bgvMax - bgvMin);
                return (uint8_t)ClampInt((int)std::round(t * 255.0f), 0, 255);
            });
            WriteGrayBitmap(acceptedPathA, scanw, scanh, [&](int x, int y) {
                const int off = x + y * scanw;
                if (off >= (int)mapAccepted.size()) return (uint8_t)0;
                return (uint8_t)ClampInt((int)std::round(mapAccepted[off] * 255.0f), 0, 255);
            });

            if (!iterBinaryHistory.empty()) {
                auto makeIterPath = [&](const std::string& base, const int idx) {
                    const size_t dot = base.find_last_of('.');
                    char suffix[64];
                    sprintf_s(suffix, ".iter_%02d", idx);
                    if (dot == std::string::npos || dot == 0) {
                        return base + suffix + ".png";
                    }
                    return base.substr(0, dot) + suffix + base.substr(dot);
                    };

                for (int i = 0; i < (int)iterBinaryHistory.size(); i++) {
                    const auto& mask = iterBinaryHistory[i];
                    const std::string outPath = makeIterPath(binaryPathA, i);
                    WriteGrayBitmap(outPath, scanw, scanh, [&](int x, int y) {
                        const int off = x + y * scanw;
                        if (off >= (int)mask.size()) return (uint8_t)0;
                        return mask[off] ? (uint8_t)255 : (uint8_t)0;
                    });
                }

                std::string csvPath = binaryPathA;
                const size_t dot = csvPath.find_last_of('.');
                if (dot == std::string::npos || dot == 0) {
                    csvPath += ".iter.csv";
                } else {
                    csvPath = csvPath.substr(0, dot) + ".iter.csv";
                }
                FILE* fiter = fopen(csvPath.c_str(), "w");
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
                std::string csvPath = binaryPathA;
                const size_t dot = csvPath.find_last_of('.');
                if (dot == std::string::npos || dot == 0) {
                    csvPath += ".promote.csv";
                } else {
                    csvPath = csvPath.substr(0, dot) + ".promote.csv";
                }
                FILE* fprom = fopen(csvPath.c_str(), "w");
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
                std::string csvPath = binaryPathA;
                const size_t dot = csvPath.find_last_of('.');
                if (dot == std::string::npos || dot == 0) {
                    csvPath += ".delta.csv";
                } else {
                    csvPath = csvPath.substr(0, dot) + ".delta.csv";
                }
                FILE* fdelta = fopen(csvPath.c_str(), "w");
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
                std::string csvPath = binaryPathA;
                const size_t dot = csvPath.find_last_of('.');
                if (dot == std::string::npos || dot == 0) {
                    csvPath += ".rectmerge.csv";
                } else {
                    csvPath = csvPath.substr(0, dot) + ".rectmerge.csv";
                }
                FILE* frect = fopen(csvPath.c_str(), "w");
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

        int getReadFrames() const { return readFrames; }
        int getTotalFrames() const { return searchFrames; }

    protected:
        /* virtual */ void onFirstFrame(AVStream *videoStream, AVFrame* frame) override {
            (void)videoStream;
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((AVPixelFormat)(frame->format));
            bitDepth = desc->comp[0].depth;
            imgw = frame->width;
            imgh = frame->height;
            scanw = RoundDownBy(std::max(32, imgw / divx), 4);
            scanh = RoundDownBy(std::max(32, imgh / divy), 4);
            scanx = std::max(0, imgw - scanw);
            scany = 0;
            radius = std::max(4, std::min(blockSize, std::min(scanw, scanh) / 4));
            if (bitDepth <= 8) {
                frameWork8.resize(scanw * scanh);
                frameWork16.clear();
                frameWork16.shrink_to_fit();
            } else {
                frameWork16.resize(scanw * scanh);
                frameWork8.clear();
                frameWork8.shrink_to_fit();
            }
            stats.resize(scanw * scanh);
            lastSampleFg.assign(scanw * scanh, 0.0f);
            lastSampleBg.assign(scanw * scanh, 0.0f);
            lastSampleValid.assign(scanw * scanh, 0);
        }

        template<typename pixel_t>
        int addFrame(const AVFrame* frame) {
            const int pitchY = frame->linesize[0] / sizeof(pixel_t);
            const pixel_t* srcY = reinterpret_cast<const pixel_t*>(frame->data[0]);
            const pixel_t maxv = (pixel_t)((1 << bitDepth) - 1);
            const float invMaxv = 1.0f / std::max(1.0f, (float)maxv);
            const float rawScale = maxv / 255.0f;
            const float thresholdRaw = threshold * rawScale;
            constexpr int kEdgeMargin = 16;
            auto& frameWork = [] (auto& w8, auto& w16) -> auto& {
                if constexpr (std::is_same_v<pixel_t, uint8_t>) {
                    return w8;
                } else {
                    return w16;
                }
            }(frameWork8, frameWork16);

            // 前処理: 輪郭保持を重視して 5x5 バイラテラルフィルタを適用。
            // 背景:
            //   3x3 平滑化(Box/Median)はロゴ輪郭を削りやすく、scoreが弱くなるケースがあった。
            //   バイラテラルでノイズを抑えつつエッジを維持する。
            // src は ROI 先頭位置までずらしたポインタを渡し、画素ループ内の余分な加算を避ける。
            constexpr int kBilateralRadius = 2;
            const float sigmaRange = std::max(6.0f * rawScale, thresholdRaw * 0.6f);
            const pixel_t* srcRoiBase = srcY + scanx + scany * pitchY;
            BilateralFilter<pixel_t, kBilateralRadius>(frameWork, srcRoiBase, pitchY, scanw, scanh, 1.4f, sigmaRange, (pixel_t)maxv, &threadPool, threadN);

            std::atomic<int> frameCount(0);
            // 同一点の重複サンプル抑制閾値。
            // 背景:
            //   レターボックス等で「ほぼ同じ fg/bg 組」が大量登録されると、
            //   有効サンプル数は増えても回帰に新情報が増えず、帯ノイズが優勢になる。
            //   ここではまず fg 一致(ほぼ同値)だけで重複を間引く。
            const float dedupThreshold = std::max(0.5f * rawScale, thresholdRaw * 0.25f);
            // ROI 全点を走査(間引きなし)。
            // 背景:
            //   初期検証で「全点チェック」の方がロゴの細部を拾いやすく、
            //   逆にサブサンプリングすると文字部が欠けやすかった。
            // ただし右上端の極近傍は境界影響が強いため、上端/右端だけ16px内側から走査する。
            // total には「実処理範囲の高さ」(scanh - 2*kEdgeMargin)を渡す。
            // RunParallelRange から渡される y0/y1 は 0 起点のローカル座標なので、
            // 実際の走査時に +kEdgeMargin して元の ROI 座標へ戻す。
            // これにより実処理範囲は従来どおり [kEdgeMargin, scanh-kEdgeMargin) となる。
            RunParallelRange(threadPool, threadN, std::max(0, scanh - 2 * kEdgeMargin), [&](int y0, int y1) {
                for (int y = y0 + kEdgeMargin; y < y1 + kEdgeMargin; y++) {
                    for (int x = kEdgeMargin; x < scanw - kEdgeMargin; x++) {
                    const int off = x + y * scanw;
                    const float fgRaw = (float)frameWork[off];
                    if (off < (int)lastSampleValid.size() && lastSampleValid[off]) {
                        // 重複抑制を先に行い、TryEstimateBgの呼び出し回数を削減して高速化する。
                        if (std::abs(lastSampleFg[off] - fgRaw) <= dedupThreshold) {
                            continue;
                        }
                    }

                    float bg = 0.0f;
                    // 背景推定不可(周辺辺が不一致など)な点は無効サンプルとして棄却。
                    // 例: ブロック辺にロゴ形状や高周波模様が入り、単色背景を仮定できない点。
                    if (!TryEstimateBg(frameWork, scanw, scanh, x, y, radius, thresholdRaw, bg)) {
                        continue;
                    }
                    AutoDetectStats& s = stats[off];
                    s.totalCandidates++;
                    const double f = (double)frameWork[off] * invMaxv;
                    const double b = (double)bg * invMaxv;

                    const float bgRaw = bg;

                    // 極端コントラスト点を棄却。
                    // 具体例:
                    //   黒帯/白飛び/フラッシュに起因する「ロゴとは無関係な巨大差分」。
                    //   これを残すと score の裾が広がり、2値化が不安定になる。
                    if (IsExtremeContrastSample(f, b)) {
                        s.rejectedExtreme++;
                        continue;
                    }
                    s.sumF += f;
                    s.sumB += b;
                    s.sumF2 += f * f;
                    s.sumB2 += b * b;
                    s.sumFB += f * b;
                    s.count++;
                    // 採用したサンプルを次回重複判定用に記録。
                    if (off < (int)lastSampleValid.size()) {
                        lastSampleFg[off] = fgRaw;
                        lastSampleBg[off] = bgRaw;
                        lastSampleValid[off] = 1;
                    }
                    // このフレームで有効だった画素数（デバッグ可視化用）。
                    frameCount.fetch_add(1, std::memory_order_relaxed);
                }
                }
                }, 4);
            return frameCount.load(std::memory_order_relaxed);
        }

        /* virtual */ bool onFrame(AVFrame* frame) override {
            if (readFrames >= searchFrames) {
                return false;
            }

            const int pixelSize = av_pix_fmt_desc_get((AVPixelFormat)(frame->format))->comp[0].step;
            int frameCount = 0;
            if (pixelSize == 1) {
                frameCount = addFrame<uint8_t>(frame);
            } else {
                frameCount = addFrame<uint16_t>(frame);
            }
            frameValidCounts.push_back(frameCount);

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
        void estimateScoreAndRect() {
            if (cb && !cb(2, 0.0f, 0.5f, readFrames, searchFrames)) {
                THROW(RuntimeException, "Cancel requested");
            }
            float thHigh = 0.0f;
            float thLow = 0.0f;

            runScoreStage(thHigh, thLow);

            runBinaryStage(thHigh, thLow);

            runRectStage();
        }

        // ステージ1: 画素ごとの統計(stats)から評価マップを作り、scoreを算出し、
        // 2値化用の初期閾値(thHigh/thLow)を決める。
        // ここでは「どの画素がロゴ候補としてどれだけ妥当か」を定量化することが目的。
        void runScoreStage(float& thHigh, float& thLow) {
            score.assign(scanw * scanh, 0.0f);
            validAB.assign(scanw * scanh, 0);
            mapA.assign(scanw * scanh, 0.0f);
            mapB.assign(scanw * scanh, 0.0f);
            mapAlpha.assign(scanw * scanh, 0.0f);
            mapLogoY.assign(scanw * scanh, 0.0f);
            mapConsistency.assign(scanw * scanh, 0.0f);
            mapBgVar.assign(scanw * scanh, 0.0f);
            mapAccepted.assign(scanw * scanh, 0.0f);
            RunParallelRange(threadPool, threadN, scanh, [&](int y0, int y1) {
                for (int y = y0; y < y1; y++) {
                    for (int x = 0; x < scanw; x++) {
                        const int i = x + y * scanw;
                        float A, B;
                        if (TryGetAB(stats[i], A, B)) {
                            validAB[i] = 1;
                            mapA[i] = A;
                            mapB[i] = B;
                            float alpha = 0.0f;
                            float logoY = 0.0f;
                            if (!TryGetAlphaLogo(A, B, alpha, logoY)) {
                                continue;
                            }
                            mapAlpha[i] = std::max(0.0f, std::min(1.0f, alpha));
                            mapLogoY[i] = std::max(0.0f, std::min(1.0f, logoY));

                            const auto& s = stats[i];
                            const double invN = (s.count > 0) ? 1.0 / s.count : 0.0;
                            const double meanF = s.sumF * invN;
                            const double meanBg = s.sumB * invN;
                            const double meanDiff = meanF - meanBg;
                            const double diff2 = (s.sumF2 - 2.0 * s.sumFB + s.sumB2) * invN;
                            const double varDiff = std::max(0.0, diff2 - meanDiff * meanDiff);
                            const double stdDiff = std::sqrt(varDiff);
                            const double consistency = std::abs(meanDiff) / (stdDiff + 1e-6);
                            const double varBg = std::max(0.0, s.sumB2 * invN - meanBg * meanBg);
                            mapConsistency[i] = (float)consistency;
                            mapBgVar[i] = (float)varBg;

                            const double extremeRejectRatio = (s.totalCandidates > 0) ? (double)s.rejectedExtreme / s.totalCandidates : 0.0;
                            const double alphaGain = std::max(0.0, std::min(1.0, (alpha - 0.005) / 0.30));
                            const double logoGain = std::max(0.0, std::min(1.0, (logoY - 0.45) / 0.30));
                            const double diffGain = std::max(0.0, std::min(1.0, meanDiff / 0.12));
                            const double consistencyGain = std::max(0.0, std::min(1.0, (consistency - 0.35) / 1.65));
                            const double bgGain = std::max(0.0, std::min(1.0, (0.080 - varBg) / 0.080));
                            const double extremeGain = std::max(0.0, std::min(1.0, 1.0 - extremeRejectRatio));
                            const float d = (float)(diffGain * (0.25 + 0.75 * consistencyGain) * (0.20 + 0.80 * alphaGain) * (0.6 + 0.4 * logoGain) * (0.50 + 0.50 * bgGain) * (0.20 + 0.80 * extremeGain));
                            if (d <= 0.0f) continue;
                            mapAccepted[i] = d;
                            score[i] = d;
                        }
                    }
                }
                });

            double sum = 0.0;
            double sum2 = 0.0;
            int count = 0;
            int validPixels = 0;
            for (int i = 0; i < scanw * scanh; i++) {
                if (validAB[i]) {
                    validPixels++;
                }
                if (!validAB[i] || score[i] <= 0) continue;
                sum += score[i];
                sum2 += score[i] * score[i];
                count++;
            }
            if (count <= 0) {
                int finalPos = 0;
                for (int i = 0; i < scanw * scanh; i++) {
                    if (!validAB[i]) continue;
                    if (score[i] > 0) finalPos++;
                }
                const auto finalStats = CalcScoreDebugStats(score, validAB);
                int sampledPixels = 0;
                int maxSampleCount = 0;
                long long totalSampleCount = 0;
                for (const auto& s : stats) {
                    if (s.count > 0) {
                        sampledPixels++;
                        totalSampleCount += s.count;
                        maxSampleCount = std::max(maxSampleCount, s.count);
                    }
                }
                int framesNonZero = 0;
                int frameMax = 0;
                long long frameSum = 0;
                for (const int v : frameValidCounts) {
                    if (v > 0) framesNonZero++;
                    frameMax = std::max(frameMax, v);
                    frameSum += v;
                }
                const double frameAvg = frameValidCounts.empty() ? 0.0 : (double)frameSum / frameValidCounts.size();
                const double sampleAvg = sampledPixels > 0 ? (double)totalSampleCount / sampledPixels : 0.0;
                THROWF(RuntimeException,
                    "Insufficient valid pixels: frames=%d/%d, roi=%dx%d@(%d,%d), "
                    "validAB=%d, scorePos=%d, sampledPixels=%d, sampleCount(avg=%.2f,max=%d), "
                    "frameValid(nonZero=%d,avg=%.2f,max=%d), "
                    "scoreFinal(pos=%d,min=%.6f,p50=%.6f,p90=%.6f,p99=%.6f,max=%.6f,mean=%.6f)",
                    readFrames, searchFrames, scanw, scanh, scanx, scany,
                    validPixels, count, sampledPixels, sampleAvg, maxSampleCount,
                    framesNonZero, frameAvg, frameMax,
                    finalPos, finalStats.minv, finalStats.p50, finalStats.p90, finalStats.p99, finalStats.maxv, finalStats.mean);
            }
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
                // score 実分布から、目標ピクセル率で閾値を逆算して決定。
                // 背景: 「統計値(mean/stddev)だけ」で閾値を作ると、
                // score が全体に低いケースで binary が全消しになりやすかった。
                // 例: ロゴ中心は出るが周辺の細字が落ちるケース(result1/result3)。
                std::vector<float> distVals;
                distVals.reserve(scanw * scanh);
                for (int i = 0; i < scanw * scanh; i++) {
                    if (!validAB[i]) continue;
                    const float v = score[i];
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

            if (cb && !cb(2, 1.0f, 0.7f, readFrames, searchFrames)) {
                THROW(RuntimeException, "Cancel requested");
            }
        }

        void runBinaryStage(const float thHigh, const float thLow);
        void runRectStage();
    };
}

namespace {
    // ステージ2: scoreを2値化してロゴ候補マスク(binary)を確定する。
    // 処理概要:
    // - high/lowのヒステリシス(seed->grow)で基本マスクを生成
    // - low側の飛び地を近縁条件で昇格
    // - 反復的な閾値緩和とdelta近縁判定で取りこぼしを補完
    // - 帯ノイズ/全消し時のフォールバックを適用
    void AutoDetectLogoReader::runBinaryStage(const float thHigh, const float thLow) {
        struct BuildBinaryDiag {
            int seedOn = 0;
            int lowOn = 0;
            int grownOn = 0;
            int promotedOn = 0;
        };

        // 2値化は score ベース、成長はシンプルな seed->low ヒステリシスを基本とする。
        auto buildBinaryFromThreshold = [&](const int iterIndex, const float highTh, const float lowTh, std::vector<uint8_t>& outBinary, BuildBinaryDiag* dbg) {
            std::vector<uint8_t> seed(scanw * scanh, 0);
            std::vector<uint8_t> lowMask(scanw * scanh, 0);
            RunParallelRange(threadPool, threadN, scanh, [&](int y0, int y1) {
                for (int y = y0; y < y1; y++) {
                    for (int x = 0; x < scanw; x++) {
                        const int off = x + y * scanw;
                        if (!validAB[off]) continue;
                        if (score[off] >= highTh) {
                            seed[off] = 1;
                        }
                        if (score[off] >= lowTh) {
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
            if (hasAnchor) {
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
                            peakScore = std::max(peakScore, (double)score[cur]);
                            sumAccepted += mapAccepted[cur];
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
                        promoteCompDebug.push_back(PromoteCompDebug{
                            iterIndex, highTh, lowTh,
                            comp.minX, comp.minY, comp.maxX, comp.maxY, comp.area, comp.compW, comp.compH,
                            comp.overlapW, comp.overlapH, comp.gapX, comp.gapY,
                            comp.peakScore, comp.meanAccepted,
                            comp.nearHorizontal, comp.nearVertical, comp.nearDiagonal, comp.nearAnchor, comp.shapeOk, comp.signalOk, 1
                            });
                    }
                    if (!anyAccepted) {
                        break;
                    }
                }
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
        };

        auto getMaskRect = [&](const std::vector<uint8_t>& mask, int& minX, int& minY, int& maxX, int& maxY) -> bool {
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
        };

        // 初回2値化（基準となる「最初の領域」）
        BuildBinaryDiag baseDiag{};
        promoteCompDebug.clear();
        deltaCompDebug.clear();
        buildBinaryFromThreshold(0, thHigh, thLow, binary, &baseDiag);
        std::vector<uint8_t> acceptedBinary = binary;
        iterBinaryHistory.clear();
        iterThresholdDebug.clear();
        auto countBinaryOn = [&](const std::vector<uint8_t>& mask) {
            int c = 0;
            for (const auto v : mask) {
                if (v) c++;
            }
            return c;
        };
        iterBinaryHistory.push_back(acceptedBinary);
        iterThresholdDebug.push_back(IterThresholdDebug{
            thHigh, thLow,
            baseDiag.seedOn, baseDiag.lowOn, baseDiag.grownOn, baseDiag.promotedOn,
            0, 0, 0, countBinaryOn(acceptedBinary), 0
            });

        int initMinX = 0, initMinY = 0, initMaxX = -1, initMaxY = -1;
        const bool hasInitRect = getMaskRect(binary, initMinX, initMinY, initMaxX, initMaxY);
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
            buildBinaryFromThreshold(iter + 1, curThHigh, curThLow, candBinary, &stepDiag);

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
                iterBinaryHistory.push_back(acceptedBinary);
                iterThresholdDebug.push_back(IterThresholdDebug{
                    curThHigh, curThLow,
                    stepDiag.seedOn, stepDiag.lowOn, stepDiag.grownOn, stepDiag.promotedOn,
                    newCount, 0, 0, countBinaryOn(acceptedBinary), 2
                    });
                continue;
            }
            if (!hasInitRect) {
                acceptedBinary.swap(candBinary);
                iterBinaryHistory.push_back(acceptedBinary);
                iterThresholdDebug.push_back(IterThresholdDebug{
                    curThHigh, curThLow,
                    stepDiag.seedOn, stepDiag.lowOn, stepDiag.grownOn, stepDiag.promotedOn,
                    newCount, 0, 0, countBinaryOn(acceptedBinary), 0
                    });
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
                    const bool nearHorizontal = overlapH >= std::max(1, (int)std::round(std::min(compH, prevH) * 0.10)) && gapX <= std::max(18, (int)std::round(prevW * 0.78));
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
                iterBinaryHistory.push_back(acceptedBinary);
                iterThresholdDebug.push_back(IterThresholdDebug{
                    curThHigh, curThLow,
                    stepDiag.seedOn, stepDiag.lowOn, stepDiag.grownOn, stepDiag.promotedOn,
                    newCount, nearCompCount, farCompCount, countBinaryOn(acceptedBinary), 1
                    });
                break;
            }
            // 近縁成分は採用し、遠方成分だけ除外して次反復へ進む。
            acceptedBinary.swap(filteredBinary);
            iterBinaryHistory.push_back(acceptedBinary);
            iterThresholdDebug.push_back(IterThresholdDebug{
                curThHigh, curThLow,
                stepDiag.seedOn, stepDiag.lowOn, stepDiag.grownOn, stepDiag.promotedOn,
                newCount, nearCompCount, farCompCount, countBinaryOn(acceptedBinary), 0
                });
        }
        binary.swap(acceptedBinary);

        int binaryOnCount = 0;
        for (int i = 0; i < scanw * scanh; i++) {
            if (binary[i]) binaryOnCount++;
        }
        if (binaryOnCount <= 0) {
            // フォールバック: 通常二値化で全消しになった場合のみ適用。
            // 背景: 閾値の厳しさや入力揺れで seed/low が空になるケース対策。
            std::vector<float> acceptedVals;
            acceptedVals.reserve(scanw * scanh);
            for (int i = 0; i < scanw * scanh; i++) {
                if (!validAB[i]) continue;
                const double consistencyNorm = std::max(0.0, std::min(1.0, (mapConsistency[i] - 0.30) / 1.70));
                if (mapAlpha[i] < 0.015f && consistencyNorm < 0.20) continue;
                acceptedVals.push_back(mapAccepted[i]);
            }
            if (!acceptedVals.empty()) {
                std::sort(acceptedVals.begin(), acceptedVals.end());
                const int idx = ClampInt((int)std::round((acceptedVals.size() - 1) * 0.995), 0, (int)acceptedVals.size() - 1);
                // 下限 0.06 は「全面ノイズ復帰」を避ける安全弁。
                const float fbTh = std::max(0.06f, acceptedVals[idx]);
                for (int i = 0; i < scanw * scanh; i++) {
                    if (!validAB[i]) continue;
                    const double consistencyNorm = std::max(0.0, std::min(1.0, (mapConsistency[i] - 0.30) / 1.70));
                    if (mapAccepted[i] >= fbTh && (mapAlpha[i] >= 0.015f || consistencyNorm >= 0.20)) {
                        binary[i] = 1;
                        binaryOnCount++;
                    }
                }
            }
        }
    }

    // ステージ3: binaryマスクから最終ロゴ矩形を決定する。
    // 処理概要:
    // - 連結成分を抽出し、形状フィルタで帯ノイズを除去
    // - 最良成分をseedに近傍成分を段階結合して文字分断を復元
    // - margin付与・サイズ制約・偶数丸めを行い最終座標(rectAbs)へ変換
    void AutoDetectLogoReader::runRectStage() {
        rectMergeDebug.clear();
        std::vector<uint8_t> xOn(scanw, 0), yOn(scanh, 0);
        for (int y = 0; y < scanh; y++) {
            for (int x = 0; x < scanw; x++) {
                const int off = x + y * scanw;
                if (!binary[off]) continue;
                xOn[x] = 1;
                yOn[y] = 1;
            }
        }

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

        std::vector<uint8_t> visited(scanw * scanh, 0);
        std::queue<int> q;
        AutoDetectRect best = coarse;
        bool hasBest = false;
        double bestScore = -1e30;
        struct CompCandidate {
            AutoDetectRect rect;
            double score;
            int area;
        };
        std::vector<CompCandidate> candidates;
        // seed選定用の厳格候補(candidates)とは別に、
        // 枠拡張用としては「binaryで出ている成分」を広く保持する。
        // 目的: 薄い文字パーツなど、seed判定では落ちる成分も最終枠では取り込めるようにする。
        std::vector<CompCandidate> mergeCandidates;
        const int minArea = std::max(24, scanw * scanh / 2048);

        for (int y = 0; y < scanh; y++) {
            for (int x = 0; x < scanw; x++) {
                const int start = x + y * scanw;
                if (!binary[start] || visited[start]) continue;
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
                        if (!binary[nidx]) {
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
                    mergeCandidates.push_back(CompCandidate{
                        comp,
                        0.0,
                        area
                        });
                }
                // 微小成分を除外。単発ノイズや孤立点を候補にしない。
                if (area < minArea) continue;

                // 3) 帯状ノイズ抑制: 極端なアスペクト比・低充填率・低コンパクト性を棄却
                const double aspect = std::max((double)comp.w / std::max(1, comp.h), (double)comp.h / std::max(1, comp.w));
                const double fillRatio = area / boxArea;
                const double compactness = (perimeter > 0) ? (4.0 * 3.14159265358979323846 * area / (perimeter * perimeter)) : 0.0;
                // 第1段階の形状フィルタ。
                // 例: 横長の細帯(レターボックス痕)や、点在ノイズ塊を落とす。
                if (aspect > 6.5) continue;
                if (fillRatio < 0.08) continue;
                if (compactness < 0.010) continue;
                // 追加の線状/帯状棄却
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
                // 第2段階の線状/帯状フィルタ。
                // isWideBand: ROI 幅の大半を占めるのに高さが薄い成分（典型的な帯）。
                // isRow/ColLineLike: 一部の行/列に画素が偏る線状成分。
                const bool isWideBand = comp.w > (int)(scanw * 0.45) && comp.h < (int)(scanh * 0.22);
                const bool isRowLineLike = maxRowRatio > 0.15 && rowCoverage < 0.52 && aspect > 2.4;
                const bool isColLineLike = maxColRatio > 0.15 && colCoverage < 0.52 && aspect > 2.4;
                if (isWideBand || isRowLineLike || isColLineLike) continue;

                const double cxComp = comp.x + comp.w * 0.5;
                const double cyComp = comp.y + comp.h * 0.5;
                // binary主体のseed選定:
                // - 面積(実体)を主軸
                // - 右上寄りを弱いpriorとして同点解消に使う
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
                candidates.push_back(CompCandidate{
                    comp,
                    compScore,
                    area
                    });

                if (compScore > bestScore) {
                    bestScore = compScore;
                    best = comp;
                    hasBest = true;
                }
            }
        }

        // 位置決定は binary成分のみで行う。
        // 背景: score/accepted依存の閾値で、良いbinaryでも枠が狭くなる事例が出たため。
        AutoDetectRect finalRect = hasBest ? best : coarse;
        if (!hasBest && !hasCoarse) {
            // 最終フォールバック: 最高 accepted 点の近傍を仮矩形にする。
            // 完全失敗で UI が空になるのを避け、デバッグ可能な状態を保つ。
            int bestIdx = -1;
            float bestV = 0.0f;
            for (int i = 0; i < scanw * scanh; i++) {
                if (!validAB[i]) continue;
                if (mapAccepted[i] > bestV) {
                    bestV = mapAccepted[i];
                    bestIdx = i;
                }
            }
            if (bestIdx >= 0 && bestV > 0.0f) {
                const int cx = bestIdx % scanw;
                const int cy = bestIdx / scanw;
                const int halfW = std::min(scanw / 4, 32);
                const int halfH = std::min(scanh / 4, 24);
                finalRect = AutoDetectRect{
                    std::max(0, cx - halfW),
                    std::max(0, cy - halfH),
                    std::min(scanw - std::max(0, cx - halfW), halfW * 2 + 1),
                    std::min(scanh - std::max(0, cy - halfH), halfH * 2 + 1)
                };
            }
        }
        if (finalRect.w <= 0 || finalRect.h <= 0) {
            THROW(RuntimeException, "Logo rect projection/CCL failed");
        }

        // seed成分に近いbinary成分を段階的に結合する。
        // 文字ロゴの分断(文字間の空白)を復元する目的で、横方向の近傍はやや広めに許容。
        if (hasBest && !mergeCandidates.empty()) {
            bool mergedAny = true;
            int mergeIter = 0;
            // seedの中心からの過剰な逸脱は抑えつつ、近傍の欠落成分は拾う。
            // 「枠が狭すぎる」問題を緩和するため、拡張時はこの上限内で近傍を結合する。
            const double bestCx = best.x + best.w * 0.5;
            const double bestCy = best.y + best.h * 0.5;
            const double maxCenterDx = std::max(80.0, best.w * 3.2);
            const double maxCenterDy = std::max(64.0, best.h * 3.2);
            while (mergedAny && mergeIter < 8) {
                mergedAny = false;
                mergeIter++;
                for (const auto& cand : mergeCandidates) {
                    // 極小成分は除外（孤立ノイズ抑制）
                    if (cand.area < 6) continue;
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
                    AutoDetectRect mergedRect{
                        std::min(finalRect.x, comp.x),
                        std::min(finalRect.y, comp.y),
                        std::max(finalRect.x + finalRect.w, comp.x + comp.w) - std::min(finalRect.x, comp.x),
                        std::max(finalRect.y + finalRect.h, comp.y + comp.h) - std::min(finalRect.y, comp.y)
                    };
                    const bool sizeGuardOk =
                        mergedRect.w <= (int)(scanw * 0.88) &&
                        mergedRect.h <= (int)(scanh * 0.62);
                    // 重なり or 近傍（横/縦/斜め）でのみ結合。
                    // 目的: 文字ロゴの分断（文字間ギャップ）を復元しつつ、
                    // 離れた別成分（無関係ノイズ）を結合しない。
                    bool accepted = true;
                    if (!(overlap || nearHorizontal || nearVertical || nearDiagonal)) accepted = false;
                    if (!(withinSeedCenterGuard || withinFinalCenterGuard)) accepted = false;
                    if (!sizeGuardOk) accepted = false;
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
                    if (!accepted) continue;
                    // マージ後サイズの上限。過剰結合で「ほぼ全域」になるのを防止。
                    if (mergedRect.x != finalRect.x || mergedRect.y != finalRect.y || mergedRect.w != finalRect.w || mergedRect.h != finalRect.h) {
                        finalRect = mergedRect;
                        mergedAny = true;
                    }
                }
            }
        }

        finalRect.x = std::max(0, finalRect.x - marginX);
        finalRect.y = std::max(0, finalRect.y - marginY);
        finalRect.w = std::min(scanw - finalRect.x, finalRect.w + marginX * 2);
        finalRect.h = std::min(scanh - finalRect.y, finalRect.h + marginY * 2);
        rectLocal = finalRect;

        AutoDetectRect absRect{
            finalRect.x + scanx,
            finalRect.y + scany,
            finalRect.w,
            finalRect.h
        };

        const double cx = absRect.x + absRect.w * 0.5;
        const double cy = absRect.y + absRect.h * 0.5;
        absRect.w = ClampInt(absRect.w, 32, 360);
        absRect.h = ClampInt(absRect.h, 32, 360);
        absRect.x = ClampInt((int)std::round(cx - absRect.w * 0.5), 0, std::max(0, imgw - absRect.w));
        absRect.y = ClampInt((int)std::round(cy - absRect.h * 0.5), 0, std::max(0, imgh - absRect.h));
        absRect.x = RoundDownBy(absRect.x, 2);
        absRect.y = RoundDownBy(absRect.y, 2);
        absRect.w = RoundUpBy(absRect.w, 2);
        absRect.h = RoundUpBy(absRect.h, 2);
        absRect.w = std::min(absRect.w, std::max(2, imgw - absRect.x));
        absRect.h = std::min(absRect.h, std::max(2, imgh - absRect.y));
        rectAbs = absRect;

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
    int* outX, int* outY, int* outW, int* outH,
    const tchar* scorePath, const tchar* binaryPath, const tchar* cclPath, const tchar* countPath, const tchar* aPath, const tchar* bPath, const tchar* alphaPath, const tchar* logoYPath, const tchar* consistencyPath, const tchar* bgVarPath, const tchar* acceptedPath,
    logo::LOGO_AUTODETECT_CB cb) {
    AutoDetectLogoReader reader(*ctx, serviceid, divx, divy, searchFrames, blockSize, threshold, marginX, marginY, threadN, cb);
    try {
        const auto rect = reader.run(srcpath);
        if (outX) *outX = rect.x;
        if (outY) *outY = rect.y;
        if (outW) *outW = rect.w;
        if (outH) *outH = rect.h;
        if (scorePath && binaryPath && cclPath && countPath && aPath && bPath && alphaPath && logoYPath && consistencyPath && bgVarPath && acceptedPath) {
            reader.writeDebug(scorePath, binaryPath, cclPath, countPath, aPath, bPath, alphaPath, logoYPath, consistencyPath, bgVarPath, acceptedPath);
        }
        return true;
    } catch (const Exception& exception) {
        if (scorePath && binaryPath && cclPath && countPath && aPath && bPath && alphaPath && logoYPath && consistencyPath && bgVarPath && acceptedPath) {
            try {
                reader.writeDebug(scorePath, binaryPath, cclPath, countPath, aPath, bPath, alphaPath, logoYPath, consistencyPath, bgVarPath, acceptedPath);
            } catch (...) {
            }
        }
        ctx->setError(exception);
    }
    return false;
}
