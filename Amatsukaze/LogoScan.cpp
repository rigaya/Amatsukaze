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
#include "zlib.h"

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

// C API for P/Invoke
extern "C" __declspec(dllexport) int ScanLogo(AMTContext * ctx,
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
