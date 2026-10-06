#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include "common.h"

// 本体の字幕描画経路を初期化し、日本語グリフを描画して利用可能か確認する。
// fontFamilyがnullならプラットフォーム既定の日本語フォントを使う。
// 戻り値は成功時1、失敗時0。診断は呼出側のUTF-8バッファに書き込む。
extern "C" AMATSUKAZE_API int CaptionPgsCheckRenderer(
    int width, int height, const char* fontFamily,
    char* diagnostic, size_t diagnosticSize) noexcept;

// PGSのキャンバスの幅・高さの上限 (PGSのオブジェクト・表示サイズの上限)
constexpr int CAPTION_PGS_CANVAS_MAX = 4096;

// 出力映像のフレームサイズとSARから、PGSのキャンバス(SARを反映した表示サイズ)を求める。
// userSARの幅・高さがともに正ならSARとしてそれを優先する。SARが不明(0以下)なら拡縮しない。
// SARが1以上なら幅を、1未満なら高さを伸ばす。長辺がCAPTION_PGS_CANVAS_MAXを超える場合は比例縮小する。
std::pair<int, int> CaptionPgsCanvasSize(int width, int height, int sarWidth, int sarHeight,
    int userSarWidth, int userSarHeight);

// CaptionPgsCanvasSizeをDLL境界越しに検証するC ABI。戻り値は成功時1、引数不正時0。
extern "C" AMATSUKAZE_API int CaptionPgsCanvasSizeForTest(int width, int height, int sarWidth, int sarHeight,
    int userSarWidth, int userSarHeight, int* canvasWidth, int* canvasHeight) noexcept;

class StreamReformInfo;
struct EncodeFileKey;

// languageは1(第1言語)または2(第2言語)。時刻写像には元の90kHz精度を保持する。
// fontFamilyが空なら既定の日本語フォントを使う。字幕がなければ空配列を返す。
// 診断のboolは警告ならtrue、補助フォント等の情報ならfalse。
using CaptionPgsDiagnostic = std::function<void(bool, const std::string&)>;

// 未解決グリフは空白へ置換し、描画できない字幕だけ省略して診断を通知する。
// 初期化やデコード等の継続不能な失敗は例外で通知する。
std::vector<uint8_t> GenerateCaptionPgs(const StreamReformInfo& reform, EncodeFileKey key,
    int language, int canvasWidth, int canvasHeight, const std::string& fontFamily,
    CaptionPgsDiagnostic diagnostic = {});
