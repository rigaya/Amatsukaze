#include "PgsEncoder.h"
#include "PgsSimd.h"

#include <algorithm>
#include <immintrin.h>
#include <limits>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace amatsukaze::pgs::simd {
namespace {
inline int FirstBit(uint32_t bits) {
#if defined(_MSC_VER) && AMT_PGS_X86
    unsigned long index;
    _BitScanForward(&index, bits);
    return static_cast<int>(index);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(bits);
#else
    int index = 0;
    while (!(bits & 1)) { bits >>= 1; index++; }
    return index;
#endif
}
inline int LastBit(uint32_t bits) {
#if defined(_MSC_VER) && AMT_PGS_X86
    unsigned long index;
    _BitScanReverse(&index, bits);
    return static_cast<int>(index);
#elif defined(__GNUC__) || defined(__clang__)
    return 31 - __builtin_clz(bits);
#else
    int index = 0;
    while (bits >>= 1) index++;
    return index;
#endif
}
}

Bounds AlphaBoundsAvx2(const Region& region) {
    Bounds bounds{region.width, region.height, 0, 0};
    const auto alpha = _mm256_set1_epi32(static_cast<int>(0xff000000u));
    const auto zero = _mm256_setzero_si256();
    static_assert(sizeof(Rgba) == 4, "RGBA画素は4byteである必要があります");
    for (int y = 0; y < region.height; y++) {
        const auto* row = region.pixels.data() + static_cast<size_t>(y) * region.width;
        int left = region.width, right = 0, x = 0;
        for (; x + 8 <= region.width; x += 8) {
            const auto pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + x));
            const auto transparent = _mm256_cmpeq_epi32(_mm256_and_si256(pixels, alpha), zero);
            const auto mask = static_cast<uint32_t>(~_mm256_movemask_ps(_mm256_castsi256_ps(transparent))) & 255;
            if (mask) {
                left = (std::min)(left, x + FirstBit(mask));
                right = x + LastBit(mask) + 1;
            }
        }
        for (; x < region.width; x++) if (row[x].a) {
            left = (std::min)(left, x); right = x + 1;
        }
        if (right) {
            bounds.left = (std::min)(bounds.left, left); bounds.right = (std::max)(bounds.right, right);
            bounds.top = (std::min)(bounds.top, y); bounds.bottom = y + 1;
        }
    }
    return bounds;
}
int RunLengthAvx2(const uint8_t* pixels, int limit) {
    const auto color = _mm256_set1_epi8(static_cast<char>(*pixels));
    int length = 0;
    for (; length + 32 <= limit; length += 32) {
        const auto block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pixels + length));
        const auto different = ~static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(block, color)));
        if (different) return length + FirstBit(different);
    }
    while (length < limit && pixels[length] == *pixels) length++;
    return length;
}

#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#endif
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
uint8_t FindNearestColorAvx2(const std::array<double, 4>& position,
    const std::array<std::array<double, 256>, 4>& coordinates, size_t count)
{
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const __m256d r = _mm256_set1_pd(position[0]);
    const __m256d g = _mm256_set1_pd(position[1]);
    const __m256d b = _mm256_set1_pd(position[2]);
    const __m256d a = _mm256_set1_pd(position[3]);
    __m256d best = _mm256_set1_pd(std::numeric_limits<double>::max());
    __m256i bestIndices = _mm256_setzero_si256();
    __m256i candidates = _mm256_set_epi64x(4, 3, 2, 1);
    for (size_t j = 0; j < count; j += 4) {
        const __m256d dr = _mm256_sub_pd(r, _mm256_load_pd(coordinates[0].data() + j));
        const __m256d dg = _mm256_sub_pd(g, _mm256_load_pd(coordinates[1].data() + j));
        const __m256d db = _mm256_sub_pd(b, _mm256_load_pd(coordinates[2].data() + j));
        const __m256d da = _mm256_sub_pd(a, _mm256_load_pd(coordinates[3].data() + j));
        // スカラー版の加算順を保ち、FMAによる丸め差を避ける。
        __m256d distance = _mm256_add_pd(_mm256_mul_pd(dr, dr), _mm256_mul_pd(dg, dg));
        distance = _mm256_add_pd(distance, _mm256_mul_pd(db, db));
        distance = _mm256_add_pd(distance, _mm256_mul_pd(da, da));
        const __m256d closer = _mm256_cmp_pd(distance, best, _CMP_LT_OQ);
        best = _mm256_blendv_pd(best, distance, closer);
        bestIndices = _mm256_castpd_si256(_mm256_blendv_pd(_mm256_castsi256_pd(bestIndices),
            _mm256_castsi256_pd(candidates), closer));
        candidates = _mm256_add_epi64(candidates, _mm256_set1_epi64x(4));
    }
    alignas(32) std::array<double, 4> distances;
    alignas(32) std::array<int64_t, 4> selected;
    _mm256_store_pd(distances.data(), best);
    _mm256_store_si256(reinterpret_cast<__m256i*>(selected.data()), bestIndices);
    size_t closest = 0;
    for (size_t lane = 1; lane < 4; lane++) {
        // 同距離なら先に登録されたパレット色を選ぶ。
        if (distances[lane] < distances[closest]
            || (distances[lane] == distances[closest] && selected[lane] < selected[closest])) {
            closest = lane;
        }
    }
    return static_cast<uint8_t>(selected[closest]);
}
#if defined(_MSC_VER)
#pragma float_control(pop)
#endif

}
