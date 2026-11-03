#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace trimadjust {

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define AMT_PREVIEW_X86 1
#else
#define AMT_PREVIEW_X86 0
#endif

enum class PreviewToneMap { Mobius, BT2446A, BT2446APreview };
enum class PreviewMatrix { BT601, BT709, BT2020, FCC, SMPTE240M };

// 縮小済みプレビュー専用。入出力はYUV420/422で、RGBやYUV444の全画面バッファは作らない。
// HLGはBT.2100の1000nit・黒0・システムガンマ1.2、SDRは100nitを仮定する。
// 色域外は輝度を保って無彩色方向へ圧縮する。制作側のSDRグレーディングの再現ではない。
class PreviewColorConverter {
    static constexpr int INTEGER_BITS = 14;
    // Q14。すべて符号付き16bitに収め、16bit×16bitの積和だけで評価する。
    struct IntegerCoefficients { int16_t y, yU, yV, uU, uV, vU, vV; };
    static const IntegerCoefficients& integerCoefficients(PreviewMatrix matrix);
    void convertIntegerScalar(const uint8_t* const src[3],const int pitch[3],int width,int height,
        int bits,const IntegerCoefficients& coefficients,bool yuv422,int firstColumn);
    static constexpr int LUT_SIZE = 4096;
    static constexpr float HDR_PEAK = 1000.f;
    static constexpr float SDR_PEAK = 100.f;
    static constexpr float MOBIUS_KNEE = .3f;
    // HLGの75%基準白をSDRの90nitへ合わせ、80nit以上を滑らかに圧縮する。
    // BT.2446A標準出力へのプレビュー用補正であり、規格そのものの変更ではない。
    static constexpr float HLG_REFERENCE_WHITE = .75f;
    static constexpr float SDR_REFERENCE_WHITE = .9f;
    static constexpr float SDR_SHOULDER = .8f;
    static float exposureShoulder(float x, float exposure) {
        const float scaled = exposure*x;
        if (scaled <= SDR_SHOULDER) return scaled;
        const float curvature = (exposure-1.f) / ((1.f-SDR_SHOULDER)*(exposure-SDR_SHOULDER));
        return SDR_SHOULDER + (scaled-SDR_SHOULDER)/(1.f+curvature*(scaled-SDR_SHOULDER));
    }
    using Table = std::array<float, LUT_SIZE + 1>;
    struct Tables {
        Table hlg, ootf, gamma24, linear24, srgb, mobius, bt2446, previewGain;
        Tables();
    };
    static const Tables& tables();
    static float unit(float x) { return (std::max)(0.f, (std::min)(1.f, x)); }
    static float lookup(const Table& table, float x) {
        const float pos = unit(x) * LUT_SIZE;
        const int index = (std::min)(int(pos), LUT_SIZE - 1);
        return table[index] + (table[index + 1] - table[index]) * (pos - index);
    }
    static uint8_t byte(float x) { return uint8_t(unit(x / 255.f) * 255.f + .5f); }
    static float luminance(float r, float g, float b) { return .2627f*r + .6780f*g + .0593f*b; }
    static float rec2020Linear(float x) {
        constexpr float alpha = 1.0992968268f, beta = .0180539685f;
        return x < 4.5f * beta ? x / 4.5f : std::pow((x + alpha - 1.f) / alpha, 1.f / .45f);
    }
    static float rec2020Signal(float x) {
        constexpr float alpha = 1.0992968268f, beta = .0180539685f;
        return x < beta ? 4.5f*x : alpha * std::pow(x, .45f) - (alpha - 1.f);
    }
    static void toRgb(float y, float u, float v, bool constantLuminance, float& r, float& g, float& b) {
        if (constantLuminance) {
            // BT.2020一定輝度は通常の3x3行列では逆変換できない。
            r = y + v * (v <= 0 ? 1.7184f : .9936f);
            b = y + u * (u <= 0 ? 1.9404f : 1.5816f);
            g = rec2020Signal((rec2020Linear(y) - .2627f*rec2020Linear(r) - .0593f*rec2020Linear(b)) / .6780f);
        } else {
            r = y + 1.4746f*v;
            b = y + 1.8814f*u;
            g = (y - .2627f*r - .0593f*b) / .6780f;
        }
    }
    static void toJpegYuv(float r, float g, float b, float& y, float& u, float& v) {
        y = .299f*r + .587f*g + .114f*b;
        u = (b - y) / 1.772f;
        v = (r - y) / 1.402f;
    }
    static void gamut709(float& r, float& g, float& b) {
        // BT.2020→BT.709は線形光で変換する。
        const float rr = 1.660491f*r - .587641f*g - .072850f*b;
        const float gg = -.124550f*r + 1.132900f*g - .008350f*b;
        const float bb = -.018151f*r - .100579f*g + 1.118730f*b;
        const float y = unit(.2126f*rr + .7152f*gg + .0722f*bb);
        float scale = 1.f;
        for (float channel : {rr, gg, bb}) {
            if (channel < 0.f) scale = (std::min)(scale, y / (y - channel));
            if (channel > 1.f) scale = (std::min)(scale, (1.f - y) / (channel - y));
        }
        r = y + scale*(rr-y); g = y + scale*(gg-y); b = y + scale*(bb-y);
    }
    void toneMap(float& r, float& g, float& b) const {
        const auto& lut = tables();
        r = lookup(lut.hlg, r); g = lookup(lut.hlg, g); b = lookup(lut.hlg, b);
        const float gain = lookup(lut.ootf, luminance(r, g, b));
        r *= gain; g *= gain; b *= gain;
        if (method != PreviewToneMap::Mobius) {
            // 表2・3をBT.2020の表示光へ適用し、SDRの非線形RGBを再構成する。
            r = lookup(lut.gamma24, r); g = lookup(lut.gamma24, g); b = lookup(lut.gamma24, b);
            const float y = luminance(r,g,b);
            const float sdrY = lookup(lut.bt2446, y);
            const float scale = y > 0.f ? sdrY / (1.1f*y) : 0.f;
            const float cb = scale*(b-y) / 1.8814f, cr = scale*(r-y) / 1.4746f;
            const float correctedY = sdrY - (std::max)(.1f*cr, 0.f);
            toRgb(correctedY, cb, cr, false, r,g,b);
            // 色域変換前の負値や1超の値は保持し、後段で輝度を保って色域内へ収める。
            auto linear = [&](float x) { return x >= 0.f && x <= 1.f ? lookup(lut.linear24,x)
                : std::copysign(std::pow(std::abs(x),2.4f),x); };
            r = linear(r); g = linear(g); b = linear(b);
            if (method == PreviewToneMap::BT2446APreview) {
                // RGB比を保つ共通ゲインで明るさを補い、白飛びを防ぐ肩を同時に適用する。
                const float correction = lookup(lut.previewGain,luminance(r,g,b));
                r *= correction; g *= correction; b *= correction;
            }
        } else {
            const float y = luminance(r,g,b);
            // LUTの入力1が1000nit、出力1が100nit。フレームごとの露出変更はしない。
            const float scale = y > 0.f ? lookup(lut.mobius,y) / y : 0.f;
            r *= scale; g *= scale; b *= scale;
        }
        gamut709(r,g,b);
        r = lookup(lut.srgb,r); g = lookup(lut.srgb,g); b = lookup(lut.srgb,b);
    }
    template<class Sample>
    void convertSamples(const uint8_t* const src[3], const int pitch[3], int width, int height,
                        int bits, bool hlg, bool constantLuminance, int firstColumn = 0, bool yuv422 = false) {
        const float factor = float(1u << (bits - 8));
        auto sample = [&](int plane, int x, int y) {
            return float(reinterpret_cast<const Sample*>(src[plane] + size_t(y)*pitch[plane])[x]) / factor;
        };
        const int chromaHeight = yuv422 ? 1 : 2;
        const size_t ySize = size_t(width)*height, cSize = ySize/(2*chromaHeight);
        uint8_t* dstY = buffer.data();
        uint8_t* dstU = dstY+ySize;
        uint8_t* dstV = dstU+cSize;
        for (int by = 0; by < height; by += chromaHeight) {
            for (int bx = firstColumn; bx < width; bx += 2) {
                const float u = (sample(1,bx/2,by/chromaHeight)-128.f)/224.f;
                const float v = (sample(2,bx/2,by/chromaHeight)-128.f)/224.f;
                float sumU = 0, sumV = 0;
                // 420は2x2、422は横2画素だけ444相当に展開する。
                // 出力色差は変換結果を平均し、丸めは最後の1回だけ行う。
                for (int dy = 0; dy < chromaHeight; dy++) for (int dx = 0; dx < 2; dx++) {
                    float r,g,b,yy,uu,vv;
                    const float y = (sample(0,bx+dx,by+dy)-16.f)/219.f;
                    toRgb(y,u,v,constantLuminance,r,g,b);
                    if (hlg) toneMap(r,g,b);
                    toJpegYuv(r,g,b,yy,uu,vv);
                    dstY[size_t(by+dy)*width+bx+dx] = byte(255.f*yy);
                    sumU += uu; sumV += vv;
                }
                const size_t ci = size_t(by/chromaHeight)*(width/2)+bx/2;
                dstU[ci] = byte(128.f + 255.f*sumU/(2*chromaHeight));
                dstV[ci] = byte(128.f + 255.f*sumV/(2*chromaHeight));
            }
        }
    }
    PreviewToneMap method;
    bool enableSimd;
    std::vector<uint8_t> buffer;
#if AMT_PREVIEW_X86
    void convertAvx2(const uint8_t* const src[3], const int pitch[3], int width, int height, int bits);
    void convertIntegerAvx2(const uint8_t* const src[3],const int pitch[3],int width,int height,
        int bits,const IntegerCoefficients& coefficients,bool yuv422);
#endif
public:
    explicit PreviewColorConverter(PreviewToneMap value = PreviewToneMap::BT2446APreview, bool useSimd = true)
        : method(value), enableSimd(useSimd) {}
    static bool hasAvx2();
    static bool hasAvx2Fma();
    // 限定レンジYUVをJPEG用BT.601・フルレンジ8bitへ整数積和で変換する。
    // 422では縦方向に触れず、420では色差を2x2ブロックで共有する。
    const std::vector<uint8_t>& convertInteger(const uint8_t* const src[3],const int pitch[3],
        int width,int height,int bits,PreviewMatrix matrix,bool yuv422 = false);
    // 接続点で値と傾きが一致し、1000nitを100nitへ写すMobius曲線。
    static float mobiusCurve(float x) {
        constexpr float peak = HDR_PEAK / SDR_PEAK, knee = MOBIUS_KNEE;
        if (x <= knee) return x;
        constexpr float span = 1.f - knee;
        constexpr float curvature = (peak - knee - span) / (span*(peak-knee));
        return knee + (x-knee) / (1.f + curvature*(x-knee));
    }
    const std::vector<uint8_t>& convert(const uint8_t* const src[3], const int pitch[3],
                                       int width, int height, int bits, bool hlg, bool constantLuminance = false, bool yuv422 = false) {
        if (width <= 0 || height <= 0 || (width&1) || (!yuv422 && (height&1)) || bits < 8 || bits > 16)
            throw std::invalid_argument("プレビュー変換のサイズまたはビット深度が不正です");
        buffer.resize(size_t(width)*height*(yuv422 ? 4 : 3)/2);
#if AMT_PREVIEW_X86
        // 命令セットの判定と方式分岐はフレームごとに1回だけ行う。
        if (enableSimd && !yuv422 && hlg && !constantLuminance && method == PreviewToneMap::BT2446APreview && hasAvx2Fma()) {
            convertAvx2(src,pitch,width,height,bits);
            // 右端の処理も通常の命令セット側で実体化し、非対応CPUの経路へAVX2を混入させない。
            if (width%8) {
                if (bits==8) convertSamples<uint8_t>(src,pitch,width,height,bits,true,false,width&~7);
                else convertSamples<uint16_t>(src,pitch,width,height,bits,true,false,width&~7);
            }
            return buffer;
        }
#endif
        if (bits == 8) convertSamples<uint8_t>(src,pitch,width,height,bits,hlg,constantLuminance,0,yuv422);
        else convertSamples<uint16_t>(src,pitch,width,height,bits,hlg,constantLuminance,0,yuv422);
        return buffer;
    }
};
}
