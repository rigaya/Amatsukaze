#pragma once

/**
* Amtasukaze Logo File
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*
* ただし、ToOutLGP()の中身の処理は
* MakKi氏の透過性ロゴ フィルタプラグインより拝借
* https://github.com/makiuchi-d/delogo-aviutl
*/
#pragma once

#include "CoreUtils.hpp"
#include "FileUtils.h"
#include "logo.h"

namespace logo {

// 拡張ヘッダの識別値とバージョン
constexpr int LOGO_EXTENDED_MAGIC = 0x12345;
constexpr int LOGO_EXTENDED_VERSION = 1;
// 色差の間引き量(log2)の上限
constexpr int LOGO_MAX_LOG_UV = 2;

struct LogoHeader {
    int magic;
    int version;
    int w, h;
    int logUVx, logUVy;
    int imgw, imgh, imgx, imgy;
    char name[255];
    int serviceId;
    int reserved[60];

    LogoHeader();

    LogoHeader(int w, int h, int logUVx, int logUVy, int imgw, int imgh, int imgx, int imgy, const std::string& name);
};

class LogoData {
protected:
    int w, h;
    int logUVx, logUVy;
    std::unique_ptr<float[]> data;
    float *aY, *aU, *aV;
    float *bY, *bU, *bV;

    static void ToYC48Y(float& y);

    static void ToYC48C(float& u);

    static void ToYV12Y(float& y);

    static void ToYV12C(float& u);

    static void ToYC48ABY(float& A, float& B);

    static void ToYC48ABC(float& A, float& B);

    static void ToOutLGP(LOGO_PIXEL& lgp, float aY, float bY, float aU, float bU, float aV, float bV);

    void WriteBaseLogo(File& file, const LogoHeader* header, const LOGO_PIXEL* data);

    void WriteExtendedLogo(File& file, const LogoHeader* header);

public:
    LogoData();

    LogoData(int w, int h, int logUVx, int logUVy);

    bool isValid() const;
    int getWidth() const;
    int getHeight() const;
    int getLogUVx() const;
    int getLogUVy() const;

    float* GetA(int plane);

    float* GetB(int plane);

    void Save(const tstring& filepath, const LogoHeader* header);
    void SaveAviUtl(const tstring& filepath, const LogoHeader* header);

    static LogoData Load(const tstring& filepath, LogoHeader* header);
};

} // namespace logo


// ネイティブ単体テストからロゴファイルの保存/読み込みを呼び出すためのC ABI。
// planesはaY, bY, aU, bU, aV, bVの順に連結した値 (Y: w*h個、UV: (w>>logUVx)*(h>>logUVy)個ずつ)。
// aviUtlOnlyが0以外ならSaveAviUtl (AviUtl互換部分のみ) で保存する。
// 成功時0、引数不正/例外時-1、出力バッファ不足時-2 (planeValueCountに必要数) を返す。
// LogoHeaderのコンストラクタはDLL外へ公開されないため、テストが確保した領域を初期化する
extern "C" AMATSUKAZE_API void InitLogoHeaderForTest(logo::LogoHeader* header, int w, int h, int logUVx, int logUVy,
    int imgw, int imgh, int imgx, int imgy, const char* name);
extern "C" AMATSUKAZE_API int SaveLogoForTest(const tchar* path, const logo::LogoHeader* header,
    const float* planes, size_t planeValueCount, int aviUtlOnly);
extern "C" AMATSUKAZE_API int LoadLogoForTest(const tchar* path, logo::LogoHeader* header,
    float* planes, size_t planeCapacity, size_t* planeValueCount);
