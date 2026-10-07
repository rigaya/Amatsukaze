#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define AMT_PGS_X86 1
#else
#define AMT_PGS_X86 0
#endif

namespace amatsukaze::pgs {
struct Region;
namespace simd {
bool HasAvx2();

struct Bounds {
    int left, top, right, bottom;
};
#if AMT_PGS_X86
Bounds AlphaBoundsAvx2(const Region& region);
int RunLengthAvx2(const uint8_t* pixels, int limit);
// coordinatesは32byte境界に揃え、4軸それぞれを256要素にする。
uint8_t FindNearestColorAvx2(const std::array<double,4>& position,
    const std::array<std::array<double,256>,4>& coordinates, size_t count);
#endif
}
}
