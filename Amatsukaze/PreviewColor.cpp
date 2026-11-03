#include "PreviewColor.h"
#include "../common/rgy_simd.h"

#if AMT_PREVIEW_X86 && defined(_MSC_VER)
#include <intrin.h>
#endif

namespace trimadjust {

const PreviewColorConverter::IntegerCoefficients& PreviewColorConverter::integerCoefficients(PreviewMatrix matrix) {
    // 入力行列の逆行列×BT.601行列と、限定→フルレンジを合成したQ14係数。
    // 列の順序はY、Y←U、Y←V、U←U、U←V、V←U、V←V。
    // 色差出力のY係数は0なので、共有色差はブロックにつき1回だけ変換できる。
    static constexpr IntegerCoefficients bt601    = { 19077,    0,    0, 18651,     0,     0, 18651 };
    static constexpr IntegerCoefficients bt709    = { 19077, 1895, 3657, 18462, -2064, -1351, 18342 };
    static constexpr IntegerCoefficients bt2020   = { 19077, 2199, 1968, 18562, -1111, -1568, 18213 };
    static constexpr IntegerCoefficients fcc      = { 19077,  151,   14, 18650,    -8,  -108, 18615 };
    static constexpr IntegerCoefficients smpte240 = { 19077, 1401, 3571, 18429, -2015, -1000, 18419 };

    switch (matrix) {
    case PreviewMatrix::BT601:     return bt601;
    case PreviewMatrix::BT709:     return bt709;
    case PreviewMatrix::BT2020:    return bt2020;
    case PreviewMatrix::FCC:       return fcc;
    case PreviewMatrix::SMPTE240M: return smpte240;
    default:
        throw std::invalid_argument("プレビューの入力行列が不正です");
    }
}

void PreviewColorConverter::convertIntegerScalar(
    const uint8_t* const src[3], const int pitch[3], int width, int height,
    int bits, const IntegerCoefficients& coefficients, bool yuv422, int firstColumn) {
    const int shift = INTEGER_BITS + bits - 8;
    const int sourceCenter = 1 << (bits - 1);
    const int rowsPerChroma = yuv422 ? 1 : 2;
    const int chromaWidth = width / 2;
    const int32_t rounding = 1 << (shift - 1);

    // 入力を符号付き16bitへ移す際のオフセットを、行列変換のバイアスに含める。
    const int32_t lumaBias = (112 << (bits - 8)) * int32_t(coefficients.y) + rounding;
    const int32_t chromaBias = (128 << shift) + rounding;
    const size_t lumaPlaneSize = size_t(width) * height;
    const size_t chromaPlaneSize = lumaPlaneSize / (2 * rowsPerChroma);
    uint8_t* dstY = buffer.data();
    uint8_t* dstU = dstY + lumaPlaneSize;
    uint8_t* dstV = dstU + chromaPlaneSize;

    auto quantize = [&](int32_t value) {
        // 負値を先にクリップし、符号付き右シフトの処理系差を避ける。
        const int32_t clipped = (std::max)(0, (std::min)(255 << shift, value));
        return uint8_t(clipped >> shift);
    };

    auto convert = [&](auto sampleTag) {
        using Sample = decltype(sampleTag);
        for (int chromaY = 0; chromaY < height / rowsPerChroma; chromaY++) {
            const Sample* srcU = reinterpret_cast<const Sample*>(src[1] + size_t(chromaY) * pitch[1]);
            const Sample* srcV = reinterpret_cast<const Sample*>(src[2] + size_t(chromaY) * pitch[2]);

            for (int chromaX = firstColumn / 2; chromaX < chromaWidth; chromaX++) {
                const int16_t sampleU = int16_t(int(srcU[chromaX]) - sourceCenter);
                const int16_t sampleV = int16_t(int(srcV[chromaX]) - sourceCenter);
                const size_t chromaIndex = size_t(chromaY) * chromaWidth + chromaX;

                // 色差は422なら横2画素、420なら2×2画素で共有する。
                const int32_t convertedU =
                    int32_t(sampleU) * coefficients.uU + int32_t(sampleV) * coefficients.uV + chromaBias;
                const int32_t convertedV =
                    int32_t(sampleU) * coefficients.vU + int32_t(sampleV) * coefficients.vV + chromaBias;
                dstU[chromaIndex] = quantize(convertedU);
                dstV[chromaIndex] = quantize(convertedV);

                // Yに加える色差成分とバイアスも、同じブロック内では共通。
                const int32_t lumaOffset =
                    int32_t(sampleU) * coefficients.yU + int32_t(sampleV) * coefficients.yV + lumaBias;
                for (int rowInBlock = 0; rowInBlock < rowsPerChroma; rowInBlock++) {
                    const int row = chromaY * rowsPerChroma + rowInBlock;
                    const Sample* srcY = reinterpret_cast<const Sample*>(src[0] + size_t(row) * pitch[0]);
                    for (int columnInBlock = 0; columnInBlock < 2; columnInBlock++) {
                        const int column = chromaX * 2 + columnInBlock;
                        const int16_t sampleY = int16_t(int(srcY[column]) - sourceCenter);
                        dstY[size_t(row) * width + column] =
                            quantize(int32_t(sampleY) * coefficients.y + lumaOffset);
                    }
                }
            }
        }
    };

    if (bits == 8) {
        convert(uint8_t{});
    } else {
        convert(uint16_t{});
    }
}

const std::vector<uint8_t>& PreviewColorConverter::convertInteger(
    const uint8_t* const src[3], const int pitch[3], int width, int height,
    int bits, PreviewMatrix matrix, bool yuv422) {
    if (width <= 0 || height <= 0 || (width & 1) || (!yuv422 && (height & 1)) || bits < 8 || bits > 16) {
        throw std::invalid_argument("整数プレビュー変換のサイズまたはビット深度が不正です");
    }

    const auto& coefficients = integerCoefficients(matrix);
    buffer.resize(size_t(width) * height * (yuv422 ? 4 : 3) / 2);

    int firstScalarColumn = 0;
#if AMT_PREVIEW_X86
    if (enableSimd && hasAvx2()) {
        convertIntegerAvx2(src, pitch, width, height, bits, coefficients, yuv422);
        firstScalarColumn = width & ~15;
    }
#endif
    if (firstScalarColumn < width) {
        convertIntegerScalar(src, pitch, width, height, bits, coefficients, yuv422, firstScalarColumn);
    }
    return buffer;
}

PreviewColorConverter::Tables::Tables() {
    const float rhoHdr = 1.f + 32.f * std::pow(HDR_PEAK / 10000.f, 1.f / 2.4f);
    const float rhoSdr = 1.f + 32.f * std::pow(SDR_PEAK / 10000.f, 1.f / 2.4f);
    constexpr float hlgA = .17883277f;
    constexpr float hlgB = .28466892f;
    constexpr float hlgC = .55991073f;

    for (int i = 0; i <= LUT_SIZE; i++) {
        const float input = float(i) / LUT_SIZE;

        // BT.2100の逆OETF。OOTFはRGB共通の輝度依存ゲインとして別に適用する。
        hlg[i] = input <= .5f
            ? input * input / 3.f
            : (std::exp((input - hlgC) / hlgA) + hlgB) / 12.f;
        ootf[i] = std::pow(input, .2f);
        gamma24[i] = std::pow(input, 1.f / 2.4f);
        linear24[i] = std::pow(input, 2.4f);
        srgb[i] = input <= .0031308f
            ? 12.92f * input
            : 1.055f * std::pow(input, 1.f / 2.4f) - .055f;
        mobius[i] = mobiusCurve(input * HDR_PEAK / SDR_PEAK);

        // ITU-R BT.2446-1の4.1、表2。表3の色補正はRGBごとに後段で行う。
        const float perceptualLuma = std::log1p((rhoHdr - 1.f) * input) / std::log(rhoHdr);
        float compressedLuma;
        if (perceptualLuma <= .7399f) {
            compressedLuma = 1.077f * perceptualLuma;
        } else if (perceptualLuma < .9909f) {
            compressedLuma = (-1.151f * perceptualLuma + 2.7811f) * perceptualLuma - .6302f;
        } else {
            compressedLuma = .5f * perceptualLuma + .5f;
        }
        bt2446[i] = (std::pow(rhoSdr, compressedLuma) - 1.f) / (rhoSdr - 1.f);
    }

    // HLGの基準白を、BT.2446A変換後の線形SDR輝度まで追跡する。
    const float sceneWhite = lookup(hlg, HLG_REFERENCE_WHITE);
    const float displayWhite = sceneWhite * lookup(ootf, sceneWhite);
    const float displayWhiteGamma = lookup(gamma24, displayWhite);
    const float mappedWhiteGamma = lookup(bt2446, displayWhiteGamma);
    const float sdrWhite = lookup(linear24, mappedWhiteGamma);

    // 固定の基準白だけで露出を決める。入力画像のヒストグラムは参照しない。
    float minExposure = 1.f;
    float maxExposure = 8.f;
    for (int i = 0; i < 32; i++) {
        const float candidate = (minExposure + maxExposure) * .5f;
        if (exposureShoulder(sdrWhite, candidate) < SDR_REFERENCE_WHITE) {
            minExposure = candidate;
        } else {
            maxExposure = candidate;
        }
    }

    const float exposure = (minExposure + maxExposure) * .5f;
    previewGain[0] = exposure;
    for (int i = 1; i <= LUT_SIZE; i++) {
        const float input = float(i) / LUT_SIZE;
        previewGain[i] = exposureShoulder(input, exposure) / input;
    }
}

const PreviewColorConverter::Tables& PreviewColorConverter::tables() {
    // 共通LUTの構築は通常の命令セット側に限定する。
    static const Tables value;
    return value;
}

bool PreviewColorConverter::hasAvx2() {
    // 既存のCPU判定はOSによるYMMレジスタ保存の対応も確認する。
    static const bool supported = !!(get_availableSIMD() & RGY_SIMD::AVX2);
    return supported;
}

bool PreviewColorConverter::hasAvx2Fma() {
    // FMAはAVX2とは別の機能ビット。OSの対応確認は既存のAVX2判定に任せる。
    static const bool supported = [] {
        if (!hasAvx2()) {
            return false;
        }
#if AMT_PREVIEW_X86 && defined(_MSC_VER)
        int registers[4];
        __cpuid(registers, 1);
        return (registers[2] & (1 << 12)) != 0;
#elif AMT_PREVIEW_X86 && (defined(__GNUC__) || defined(__clang__))
        return bool(__builtin_cpu_supports("fma"));
#else
        return false;
#endif
    }();
    return supported;
}
}
