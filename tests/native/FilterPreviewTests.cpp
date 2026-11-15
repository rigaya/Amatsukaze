#include "FilterPreview.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <filesystem>
#include <chrono>
extern "C" AMTContext* AMTContext_Create();
extern "C" void ATMContext_Delete(AMTContext*);
extern "C" const char* AMTContext_GetError(AMTContext*);

int main() {
    const auto path = std::filesystem::temp_directory_path() / ("amatsukaze_filter_preview_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".avs");
    { std::ofstream file(path); file << "ClearAutoloadDirs()\nBlankClip(length=4, width=32, height=24, fps=25, pixel_type=\"YV12\", color_yuv=$108080)\n"; }
    AMTContext* ctx = AMTContext_Create();
    const auto datPath = path.string() + ".dat";
    VideoFormat source{};
    source.progressive = false;
    source.sarWidth = 4; source.sarHeight = 3;
    source.colorPrimaries = 1; source.transferCharacteristics = 6; source.colorSpace = 9;
    {
        std::ofstream file(datPath, std::ios::binary);
        const int64_t emptyLength = 0;
        file.write(reinterpret_cast<const char*>(&emptyLength), sizeof(emptyLength));
        file.write(reinterpret_cast<const char*>(&emptyLength), sizeof(emptyLength));
        file.write(reinterpret_cast<const char*>(&source), sizeof(source));
    }
    FilterPreviewSourceFormat sourceFormat{};
    if (!FilterPreview_GetSourceFormat(ctx, std::filesystem::path(datPath).c_str(), &sourceFormat) ||
        sourceFormat.progressive != 0 || sourceFormat.sarWidth != 4 || sourceFormat.sarHeight != 3 ||
        sourceFormat.colorPrimaries != 1 || sourceFormat.transferCharacteristics != 6 || sourceFormat.colorSpace != 9) {
        std::fprintf(stderr, "映像形式の取得に失敗: %s\n", AMTContext_GetError(ctx));
        ATMContext_Delete(ctx); return 1;
    }
    if (FilterPreview_GetSourceFormat(ctx, _T("存在しない.dat"), &sourceFormat) || !*AMTContext_GetError(ctx)) {
        std::fprintf(stderr, "存在しない入力のエラーがありません\n");
        ATMContext_Delete(ctx); return 1;
    }
    std::filesystem::remove(datPath);
    FilterPreviewInfo info{};
    auto preview = FilterPreview_Open(ctx, path.c_str(), &info);
    auto fail = [&](const char* msg) { std::fprintf(stderr, "%s: %s\n", msg, AMTContext_GetError(ctx)); };
    bool ok = preview != nullptr;
    if (!ok) fail("Open");
    if (ok && (info.numFrames != 4 || info.fpsNum != 25 || info.fpsDen != 1 || info.width != 32 || info.height != 24 || info.bitDepth != 8 || info.pixelType != 1)) { fail("映像情報"); ok = false; }
    const uint8_t* planes[3]{}; int pitches[3]{};
    if (ok && (!FilterPreview_GetFrame(preview, 2, planes, pitches) || !planes[0] || !planes[1] || !planes[2] || pitches[0] < 32 || pitches[1] < 16 || pitches[2] < 16)) { fail("フレーム取得"); ok = false; }
    int type = -1, duration = -1;
    if (ok && (FilterPreview_GetKFMProps(preview, 2, &type, &duration) != 0 || type != 0 || duration != 0)) { fail("プロパティなし"); ok = false; }
    if (ok && (FilterPreview_GetFrame(preview, 4, planes, pitches) != 0 || !*AMTContext_GetError(ctx))) { fail("範囲外"); ok = false; }
    if (ok && FilterPreview_GetKFMProps(preview, -1, &type, &duration) != -1) { fail("プロパティ範囲外"); ok = false; }
    FilterPreview_Close(preview);
#if !(defined(_WIN32) || defined(_WIN64))
    // AviSynth+でプロパティを合成し、値の取得も検証する。
    { std::ofstream file(path); file << "ClearAutoloadDirs()\nBlankClip(length=4, width=32, height=24, pixel_type=\"YV12\")\nPropSet(\"KFM_FrameType\", 3)\nPropSet(\"KFM_Duration\", 2)\nPropSet(\"_SARNum\", 4)\nPropSet(\"_SARDen\", 3)\n"; }
    preview = FilterPreview_Open(ctx, path.c_str(), &info);
    if (!preview) { fail("プロパティ付きOpen"); ok = false; }
    if (preview && (FilterPreview_GetKFMProps(preview, 1, &type, &duration) != 1 || type != 3 || duration != 2 || info.sarW != 4 || info.sarH != 3)) { fail("プロパティ取得"); ok = false; }
    FilterPreview_Close(preview);
    // 10bitの平面データを変換せず取得する。
    { std::ofstream file(path); file << "ClearAutoloadDirs()\nBlankClip(length=2, width=32, height=24, pixel_type=\"YUV420P10\")\n"; }
    preview = FilterPreview_Open(ctx, path.c_str(), &info);
    if (!preview || info.bitDepth != 10 || info.pixelType != FILTER_PREVIEW_YUV420 || !FilterPreview_GetFrame(preview, 0, planes, pitches) || pitches[0] < 64) { fail("10bitフレーム"); ok = false; }
    FilterPreview_Close(preview);
#endif
    // 構文エラーでも環境破棄後にエラーメッセージを参照できる。
    { std::ofstream file(path); file << "存在しない関数()\n"; }
    preview = FilterPreview_Open(ctx, path.c_str(), &info);
    if (preview || !*AMTContext_GetError(ctx)) { fail("不正なスクリプト"); ok = false; }
    FilterPreview_Close(preview);
    ATMContext_Delete(ctx);
    std::error_code ignored; std::filesystem::remove(path, ignored);
    return ok ? 0 : 1;
}
