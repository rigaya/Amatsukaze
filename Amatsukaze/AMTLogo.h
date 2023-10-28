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

    static LogoData Load(const tstring& filepath, LogoHeader* header);
};

} // namespace logo

