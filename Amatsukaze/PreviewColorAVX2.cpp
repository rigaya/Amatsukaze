// 暗黙の演算融合は止め、FMA化する積和はヘルパーで明示する。
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

#include "PreviewColor.h"
#include <immintrin.h>
#include <cstring>

namespace trimadjust {
namespace {

struct LutView {
    const float* hlg;
    const float* ootf;
    const float* gamma24;
    const float* linear24;
    const float* srgb;
    const float* bt2446;
    const float* previewGain;
    int size;
};

// 浮動小数点の演算と積和を、式として追いやすく記述するための補助。
inline __m256 splat(float value) {
    return _mm256_set1_ps(value);
}
inline __m256 add(__m256 a, __m256 b) {
    return _mm256_add_ps(a, b);
}
inline __m256 sub(__m256 a, __m256 b) {
    return _mm256_sub_ps(a, b);
}
inline __m256 mul(__m256 a, __m256 b) {
    return _mm256_mul_ps(a, b);
}
inline __m256 div(__m256 a, __m256 b) {
    return _mm256_div_ps(a, b);
}
inline __m256 fmadd(__m256 a, __m256 b, __m256 c) {
    return _mm256_fmadd_ps(a, b, c);
}
inline __m256 fmsub(__m256 a, __m256 b, __m256 c) {
    return _mm256_fmsub_ps(a, b, c);
}
inline __m256 clampUnit(__m256 value) {
    return _mm256_max_ps(splat(0), _mm256_min_ps(splat(1), value));
}

inline __m256 lookupLut(const float* table, __m256 input, int size) {
    const __m256 position = mul(clampUnit(input), splat(float(size)));
    const __m256i index = _mm256_min_epi32(
        _mm256_cvttps_epi32(position), _mm256_set1_epi32(size - 1));
    const __m256i nextIndex = _mm256_add_epi32(index, _mm256_set1_epi32(1));

    const __m256 currentValue = _mm256_i32gather_ps(table, index, 4);
    const __m256 nextValue = _mm256_i32gather_ps(table, nextIndex, 4);
    const __m256 fraction = sub(position, _mm256_cvtepi32_ps(index));
    return fmadd(sub(nextValue, currentValue), fraction, currentValue);
}

inline __m256 luminance(__m256 r, __m256 g, __m256 b) {
    const __m256 redGreen = fmadd(splat(.6780f), g, mul(splat(.2627f), r));
    return fmadd(splat(.0593f), b, redGreen);
}

inline __m256 linearizeSdr(__m256 input, const LutView& lut) {
    __m256 result = lookupLut(lut.linear24, input, lut.size);
    const __m256 belowRange = _mm256_cmp_ps(input, splat(0), _CMP_LT_OQ);
    const __m256 aboveRange = _mm256_cmp_ps(input, splat(1), _CMP_GT_OQ);
    const int outsideMask = _mm256_movemask_ps(_mm256_or_ps(belowRange, aboveRange));

    if (outsideMask) {
        // 色域外の成分だけは近似せず、元と同じ累乗を用いる。
        alignas(32) float inputLanes[8];
        alignas(32) float outputLanes[8];
        _mm256_store_ps(inputLanes, input);
        _mm256_store_ps(outputLanes, result);

        for (int lane = 0; lane < 8; lane++) {
            if (outsideMask & (1 << lane)) {
                outputLanes[lane] = std::copysign(
                    std::pow(std::abs(inputLanes[lane]), 2.4f), inputLanes[lane]);
            }
        }
        result = _mm256_load_ps(outputLanes);
    }
    return result;
}

inline void toneMapVector(__m256& r, __m256& g, __m256& b, const LutView& lut) {
    // 1. HLGを線形表示光へ戻し、BT.2446A用のガンマ領域へ移す。
    r = lookupLut(lut.hlg, r, lut.size);
    g = lookupLut(lut.hlg, g, lut.size);
    b = lookupLut(lut.hlg, b, lut.size);
    const __m256 ootfGain = lookupLut(lut.ootf, luminance(r, g, b), lut.size);
    r = lookupLut(lut.gamma24, mul(r, ootfGain), lut.size);
    g = lookupLut(lut.gamma24, mul(g, ootfGain), lut.size);
    b = lookupLut(lut.gamma24, mul(b, ootfGain), lut.size);

    // 2. BT.2446Aの輝度圧縮と色差補正を行い、非線形RGBを再構成する。
    const __m256 hdrLuma = luminance(r, g, b);
    const __m256 mappedLuma = lookupLut(lut.bt2446, hdrLuma, lut.size);
    const __m256 positiveLumaMask = _mm256_cmp_ps(hdrLuma, splat(0), _CMP_GT_OQ);
    const __m256 safeLuma = _mm256_blendv_ps(splat(1), hdrLuma, positiveLumaMask);
    const __m256 chromaScale = _mm256_and_ps(
        positiveLumaMask, div(mappedLuma, mul(splat(1.1f), safeLuma)));
    const __m256 cb = div(mul(chromaScale, sub(b, hdrLuma)), splat(1.8814f));
    const __m256 cr = div(mul(chromaScale, sub(r, hdrLuma)), splat(1.4746f));
    const __m256 correctedLuma = sub(mappedLuma, _mm256_max_ps(mul(splat(.1f), cr), splat(0)));
    r = fmadd(splat(1.4746f), cr, correctedLuma);
    b = fmadd(splat(1.8814f), cb, correctedLuma);
    const __m256 greenNumerator = fmadd(splat(-.2627f), r, correctedLuma);
    g = div(fmadd(splat(-.0593f), b, greenNumerator), splat(.6780f));

    // 3. 線形光へ戻し、RGB共通のゲインでプレビューの基準白を補正する。
    r = linearizeSdr(r, lut);
    g = linearizeSdr(g, lut);
    b = linearizeSdr(b, lut);
    const __m256 previewGain = lookupLut(lut.previewGain, luminance(r, g, b), lut.size);
    r = mul(r, previewGain);
    g = mul(g, previewGain);
    b = mul(b, previewGain);

    // 4. 線形光のままBT.2020からBT.709へ色域変換する。
    const __m256 redBase = fmsub(splat(1.660491f), r, mul(splat(.587641f), g));
    const __m256 greenBase = fmadd(splat(1.132900f), g, mul(splat(-.124550f), r));
    const __m256 blueBase = fmsub(splat(-.018151f), r, mul(splat(.100579f), g));
    const __m256 red709 = fmadd(splat(-.072850f), b, redBase);
    const __m256 green709 = fmadd(splat(-.008350f), b, greenBase);
    const __m256 blue709 = fmadd(splat(1.118730f), b, blueBase);
    const __m256 grayRedGreen = fmadd(splat(.7152f), green709, mul(splat(.2126f), red709));
    const __m256 gray = clampUnit(fmadd(splat(.0722f), blue709, grayRedGreen));

    // 5. 色域外だけを無彩色方向へ縮める。不要なレーンの0除算は避ける。
    __m256 gamutScale = splat(1);
    for (__m256 channel : { red709, green709, blue709 }) {
        const __m256 negativeMask = _mm256_cmp_ps(channel, splat(0), _CMP_LT_OQ);
        const __m256 brightMask = _mm256_cmp_ps(channel, splat(1), _CMP_GT_OQ);
        if (_mm256_movemask_ps(_mm256_or_ps(negativeMask, brightMask))) {
            const __m256 negativeDenominator = _mm256_blendv_ps(splat(1), sub(gray, channel), negativeMask);
            const __m256 negativeScale = div(gray, negativeDenominator);
            gamutScale = _mm256_min_ps(
                gamutScale, _mm256_blendv_ps(splat(1), negativeScale, negativeMask));

            const __m256 brightDenominator = _mm256_blendv_ps(splat(1), sub(channel, gray), brightMask);
            const __m256 brightScale = div(sub(splat(1), gray), brightDenominator);
            gamutScale = _mm256_min_ps(
                gamutScale, _mm256_blendv_ps(splat(1), brightScale, brightMask));
        }
    }

    // 6. 色域内へ収めた線形RGBをsRGBで符号化する。
    r = lookupLut(lut.srgb, fmadd(gamutScale, sub(red709, gray), gray), lut.size);
    g = lookupLut(lut.srgb, fmadd(gamutScale, sub(green709, gray), gray), lut.size);
    b = lookupLut(lut.srgb, fmadd(gamutScale, sub(blue709, gray), gray), lut.size);
}

inline __m256i quantizeBytes(__m256 input) {
    // 0〜255へクリップし、0.5を加えて切り捨てる。
    const __m256 clipped = clampUnit(div(input, splat(255)));
    const __m256 rounded = fmadd(clipped, splat(255), splat(.5f));
    return _mm256_cvttps_epi32(rounded);
}

inline void storeLuma8(uint8_t* dst, __m256 input) {
    const __m256i values = quantizeBytes(input);
    const __m128i low = _mm256_castsi256_si128(values);
    const __m128i high = _mm256_extracti128_si256(values, 1);
    const __m128i words = _mm_packus_epi32(low, high);
    const __m128i packed = _mm_packus_epi16(words, _mm_setzero_si128());
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), packed);
}

inline void storeChroma420(uint8_t* dst, __m256 top, __m256 bottom) {
    // 左上→右上→左下→右下の加算順を守り、色差を4画素で平均する。
    // 最後に偶数レーンだけを取り出すため、奇数レーンの加算順は結果に影響しない。
    const __m256 topPairSum = add(top, _mm256_permute_ps(top, 0xb1));
    __m256 sum = add(topPairSum, bottom);
    sum = add(sum, _mm256_permute_ps(bottom, 0xb1));
    const __m256 averaged = add(splat(128), div(mul(splat(255), sum), splat(4)));
    const __m256i evenLanes = _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6);
    const __m256i values = quantizeBytes(_mm256_permutevar8x32_ps(averaged, evenLanes));

    const __m128i words = _mm_packus_epi32(_mm256_castsi256_si128(values), _mm_setzero_si128());
    const __m128i bytes = _mm_packus_epi16(words, _mm_setzero_si128());
    const uint32_t packed = uint32_t(_mm_cvtsi128_si32(bytes));
    std::memcpy(dst, &packed, sizeof(packed));
}

template<class Sample>
inline __m256 loadLuma8(const Sample* src) {
    __m256i values;
    if constexpr (sizeof(Sample) == 1) {
        const __m128i bytes = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src));
        values = _mm256_cvtepu8_epi32(bytes);
    } else {
        const __m128i words = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        values = _mm256_cvtepu16_epi32(words);
    }
    return _mm256_cvtepi32_ps(values);
}

template<class Sample>
inline __m256 loadChroma4(const Sample* src) {
    __m128i source;
    if constexpr (sizeof(Sample) == 1) {
        uint32_t packed;
        std::memcpy(&packed, src, sizeof(packed));
        source = _mm_cvtsi32_si128(int(packed));
    } else {
        source = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src));
    }

    __m256i values;
    if constexpr (sizeof(Sample) == 1) {
        values = _mm256_cvtepu8_epi32(source);
    } else {
        values = _mm256_cvtepu16_epi32(source);
    }

    // 各色差を隣接2画素へ複製する。上段・下段でも同じ色差を共有する。
    const __m256i duplicatePairs = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3);
    return _mm256_cvtepi32_ps(_mm256_permutevar8x32_epi32(values, duplicatePairs));
}

template<class Sample>
inline __m128i loadCenteredInteger8(const Sample* src, __m128i center) {
    __m128i values;
    if constexpr (sizeof(Sample) == 1) {
        const __m128i bytes = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src));
        values = _mm_cvtepu8_epi16(bytes);
    } else {
        values = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
    }
    return _mm_sub_epi16(values, center);
}

inline __m256i coefficientPair(int16_t lowCoefficient, int16_t highCoefficient) {
    const uint32_t packed =
        uint32_t(uint16_t(lowCoefficient)) | (uint32_t(uint16_t(highCoefficient)) << 16);
    return _mm256_set1_epi32(int(packed));
}

inline void storeInteger8(uint8_t* dst, __m256i values, __m128i shift) {
    const __m256i shifted = _mm256_sra_epi32(values, shift);
    const __m128i low = _mm256_castsi256_si128(shifted);
    const __m128i high = _mm256_extracti128_si256(shifted, 1);
    const __m128i words = _mm_packus_epi32(low, high);
    const __m128i packed = _mm_packus_epi16(words, _mm_setzero_si128());
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), packed);
}

}

void PreviewColorConverter::convertAvx2(
    const uint8_t* const src[3], const int pitch[3], int width, int height, int bits) {
    const auto& data = tables();
    const LutView lut = {
        data.hlg.data(),
        data.ootf.data(),
        data.gamma24.data(),
        data.linear24.data(),
        data.srgb.data(),
        data.bt2446.data(),
        data.previewGain.data(),
        LUT_SIZE
    };

    const size_t lumaPlaneSize = size_t(width) * height;
    uint8_t* dstY = buffer.data();
    uint8_t* dstU = dstY + lumaPlaneSize;
    uint8_t* dstV = dstU + lumaPlaneSize / 4;
    const __m256 sampleScale = splat(float(1u << (bits - 8)));

    auto convert = [&](auto sampleTag) {
        using Sample = decltype(sampleTag);
        for (int blockY = 0; blockY < height; blockY += 2) {
            const Sample* srcYTop = reinterpret_cast<const Sample*>(src[0] + size_t(blockY) * pitch[0]);
            const Sample* srcYBottom = reinterpret_cast<const Sample*>(src[0] + size_t(blockY + 1) * pitch[0]);
            const Sample* srcU = reinterpret_cast<const Sample*>(src[1] + size_t(blockY / 2) * pitch[1]);
            const Sample* srcV = reinterpret_cast<const Sample*>(src[2] + size_t(blockY / 2) * pitch[2]);

            for (int blockX = 0; blockX + 8 <= width; blockX += 8) {
                // 上下2行で共有する4組の色差を、8画素分へ展開する。
                const __m256 u = div(sub(div(loadChroma4(srcU + blockX / 2), sampleScale), splat(128)), splat(224));
                const __m256 v = div(sub(div(loadChroma4(srcV + blockX / 2), sampleScale), splat(128)), splat(224));
                __m256 outputU[2];
                __m256 outputV[2];

                for (int rowInBlock = 0; rowInBlock < 2; rowInBlock++) {
                    const Sample* srcY = (rowInBlock ? srcYBottom : srcYTop) + blockX;
                    const __m256 y = div(sub(div(loadLuma8(srcY), sampleScale), splat(16)), splat(219));

                    // BT.2020逆行列→トーンマッピング→JPEG用BT.601行列。
                    __m256 r = fmadd(splat(1.4746f), v, y);
                    __m256 b = fmadd(splat(1.8814f), u, y);
                    const __m256 greenNumerator = fmadd(splat(-.2627f), r, y);
                    __m256 g = div(fmadd(splat(-.0593f), b, greenNumerator), splat(.6780f));
                    toneMapVector(r, g, b, lut);
                    const __m256 redGreen = fmadd(splat(.587f), g, mul(splat(.299f), r));
                    const __m256 outputY = fmadd(splat(.114f), b, redGreen);
                    outputU[rowInBlock] = div(sub(b, outputY), splat(1.772f));
                    outputV[rowInBlock] = div(sub(r, outputY), splat(1.402f));

                    uint8_t* dstRow = dstY + size_t(blockY + rowInBlock) * width + blockX;
                    storeLuma8(dstRow, mul(splat(255), outputY));
                }

                const size_t chromaIndex = size_t(blockY / 2) * (width / 2) + blockX / 2;
                storeChroma420(dstU + chromaIndex, outputU[0], outputU[1]);
                storeChroma420(dstV + chromaIndex, outputV[0], outputV[1]);
            }
        }
    };

    if (bits == 8) {
        convert(uint8_t{});
    } else {
        convert(uint16_t{});
    }
}

void PreviewColorConverter::convertIntegerAvx2(
    const uint8_t* const src[3], const int pitch[3], int width, int height,
    int bits, const IntegerCoefficients& coefficients, bool yuv422) {
    const int shift = INTEGER_BITS + bits - 8;
    const int rowsPerChroma = yuv422 ? 1 : 2;
    const int chromaWidth = width / 2;
    const int32_t rounding = 1 << (shift - 1);
    const __m128i shiftCount = _mm_cvtsi32_si128(shift);
    const __m128i sourceCenter = _mm_set1_epi16(short(1u << (bits - 1)));

    // オフセットと丸めは、積和の後に加えるバイアスへまとめる。
    const __m256i lumaBias = _mm256_set1_epi32((112 << (bits - 8)) * int32_t(coefficients.y) + rounding);
    const __m256i chromaBias = _mm256_set1_epi32((128 << shift) + rounding);
    const __m256i lumaCoefficient = coefficientPair(coefficients.y, 0);
    const __m256i lumaUvCoefficients = coefficientPair(coefficients.yU, coefficients.yV);
    const __m256i uCoefficients = coefficientPair(coefficients.uU, coefficients.uV);
    const __m256i vCoefficients = coefficientPair(coefficients.vU, coefficients.vV);

    // 8組の色差補正を、16画素のYに対応する前半・後半へ複製する。
    const __m256i duplicateLow = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3);
    const __m256i duplicateHigh = _mm256_setr_epi32(4, 4, 5, 5, 6, 6, 7, 7);
    const size_t lumaPlaneSize = size_t(width) * height;
    const size_t chromaPlaneSize = lumaPlaneSize / (2 * rowsPerChroma);
    uint8_t* dstY = buffer.data();
    uint8_t* dstU = dstY + lumaPlaneSize;
    uint8_t* dstV = dstU + chromaPlaneSize;

    auto convert = [&](auto sampleTag) {
        using Sample = decltype(sampleTag);
        for (int chromaY = 0; chromaY < height / rowsPerChroma; chromaY++) {
            const Sample* srcU = reinterpret_cast<const Sample*>(src[1] + size_t(chromaY) * pitch[1]);
            const Sample* srcV = reinterpret_cast<const Sample*>(src[2] + size_t(chromaY) * pitch[2]);

            for (int blockX = 0; blockX + 16 <= width; blockX += 16) {
                const __m128i sampleU = loadCenteredInteger8(srcU + blockX / 2, sourceCenter);
                const __m128i sampleV = loadCenteredInteger8(srcV + blockX / 2, sourceCenter);
                const __m128i uvLow = _mm_unpacklo_epi16(sampleU, sampleV);
                const __m128i uvHigh = _mm_unpackhi_epi16(sampleU, sampleV);
                const __m256i uvPairs = _mm256_set_m128i(uvHigh, uvLow);

                // [U0,V0,U1,V1,...]と係数の組で、各色差を1回ずつ積和変換する。
                const __m256i convertedU =
                    _mm256_add_epi32(_mm256_madd_epi16(uvPairs, uCoefficients), chromaBias);
                const __m256i convertedV =
                    _mm256_add_epi32(_mm256_madd_epi16(uvPairs, vCoefficients), chromaBias);
                const size_t chromaIndex = size_t(chromaY) * chromaWidth + blockX / 2;
                storeInteger8(dstU + chromaIndex, convertedU, shiftCount);
                storeInteger8(dstV + chromaIndex, convertedV, shiftCount);

                const __m256i lumaOffset =
                    _mm256_add_epi32(_mm256_madd_epi16(uvPairs, lumaUvCoefficients), lumaBias);
                const __m256i lumaOffsetLow = _mm256_permutevar8x32_epi32(lumaOffset, duplicateLow);
                const __m256i lumaOffsetHigh = _mm256_permutevar8x32_epi32(lumaOffset, duplicateHigh);
                for (int rowInBlock = 0; rowInBlock < rowsPerChroma; rowInBlock++) {
                    const int row = chromaY * rowsPerChroma + rowInBlock;
                    const Sample* srcY = reinterpret_cast<const Sample*>(src[0] + size_t(row) * pitch[0]) + blockX;

                    // Yをゼロと組にし、輝度の乗算も16bit積和で行う。VPMULLDは使わない。
                    const __m256i lumaPairsLow = _mm256_cvtepu16_epi32(loadCenteredInteger8(srcY, sourceCenter));
                    const __m256i lumaPairsHigh = _mm256_cvtepu16_epi32(loadCenteredInteger8(srcY + 8, sourceCenter));
                    const __m256i convertedYLow =
                        _mm256_add_epi32(_mm256_madd_epi16(lumaPairsLow, lumaCoefficient), lumaOffsetLow);
                    const __m256i convertedYHigh =
                        _mm256_add_epi32(_mm256_madd_epi16(lumaPairsHigh, lumaCoefficient), lumaOffsetHigh);
                    uint8_t* dstRow = dstY + size_t(row) * width + blockX;
                    storeInteger8(dstRow, convertedYLow, shiftCount);
                    storeInteger8(dstRow + 8, convertedYHigh, shiftCount);
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
}
