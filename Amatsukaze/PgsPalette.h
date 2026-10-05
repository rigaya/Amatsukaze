#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace amatsukaze::pgs {

struct Rgba {
    uint8_t r, g, b, a;
};

struct PaletteEntry {
    Rgba rgba;
    uint8_t y, cb, cr;
};

struct IndexedImage {
    int width, height;
    std::vector<uint8_t> pixels;
};

struct PaletteResult {
    std::vector<PaletteEntry> entries;
    std::vector<IndexedImage> images;
};

// 全画像でパレットを共有し、完全透明な画素は常にインデックス0にする。
// 入力RGBAはstraightアルファ。画像数・寸法・画素数の不整合は例外にする。
PaletteResult MakePalette(const std::vector<std::vector<Rgba>>& images,
    const std::vector<std::pair<int, int>>& sizes, int canvasHeight);

}
