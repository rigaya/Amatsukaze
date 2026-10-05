#pragma once

#include "PgsPalette.h"
#include <cstdint>
#include <filesystem>
#include <vector>

namespace amatsukaze::pgs {

struct Region {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    // 行優先のstraightアルファRGBA。重なる領域は配列順に上へ重ねる。
    std::vector<Rgba> pixels;
};

struct Event {
    int64_t start90k = 0;
    int64_t end90k = 0;
    std::vector<Region> regions;
};

class PgsEncoder {
public:
    // イベントは時系列の非重複区間[start90k,end90k)。空画像も表示の消去に使える。
    // キャンバスは幅・高さ1～4096。キャンバス外の領域、不正サイズ、PGSの長さ上限超過は例外で通知する。
    // SUPのPTSフィールドは32ビットなので、長時間の時刻はその幅で折り返す。
    static std::vector<uint8_t> Encode(int canvasWidth, int canvasHeight, const std::vector<Event>& events);
    static void WriteFile(const std::filesystem::path& path, int canvasWidth, int canvasHeight,
        const std::vector<Event>& events);

    // 行ごとに符号化する。単体検証用に、PGSオブジェクトの幅上限とは独立して使える。
    static std::vector<uint8_t> EncodeRle(const std::vector<uint8_t>& pixels, int width, int height);
};

}
