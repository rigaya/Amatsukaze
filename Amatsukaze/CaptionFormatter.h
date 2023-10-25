#pragma once

/**
* Caption Formatter
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "StreamReform.h"

class CaptionASSFormatter : public AMTObject {
public:
    CaptionASSFormatter(AMTContext& ctx);

    std::wstring generate(const std::vector<OutCaptionLine>& lines);

private:
    struct FormatState {
        int x, y;
        float fsx, fsy;
        int spacing;
        CLUT_DAT_DLL textColor;
        CLUT_DAT_DLL backColor;
        int style;
    };

    StringBuilderW sb;
    StringBuilderW attr;
    int PlayResX;
    int PlayResY;
    float DefFontSize;

    FormatState initialState;
    FormatState curState;

    void header();

    void item(const OutCaptionLine& line);

    void fragment(float scalex, float scaley, const std::wstring& text, const CaptionFormat& fmt);

    void time(double t);

    void setPos(int x, int y);

    void setColor(CLUT_DAT_DLL textColor, CLUT_DAT_DLL backColor);

    void setFontSize(float fsx, float fsy);

    void setSpacing(int spacing);

    void setStyle(int style);
};

class CaptionSRTFormatter : public AMTObject {
public:
    CaptionSRTFormatter(AMTContext& ctx);

    std::wstring generate(const std::vector<OutCaptionLine>& lines);

private:
    StringBuilderW sb;
    StringBuilderW linebuf;
    int subIndex;
    double prevEnd;
    float prevPosY;

    void pushLine();

    void time(double t);

    void item(const OutCaptionLine& line);

};

