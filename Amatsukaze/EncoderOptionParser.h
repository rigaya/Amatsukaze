#pragma once

/**
* Encoder Option Parser
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <string>
#include <regex>

#include "StringUtils.h"
#include "TranscodeSetting.h"
#include "rgy_util.h"
#include "AviSynthWrapper.h"

enum ENUM_ENCODER_DEINT {
    ENCODER_DEINT_NONE,
    ENCODER_DEINT_30P,
    ENCODER_DEINT_24P,
    ENCODER_DEINT_60P,
    ENCODER_DEINT_VFR
};

struct EncoderOptionInfo {
    VIDEO_STREAM_FORMAT format;
    ENUM_ENCODER_DEINT deint;
    bool afsTimecode;
    int selectEvery;
    std::string rcMode;
    double rcModeValue[3];
    int parallel;
};

struct EncoderRCMode {
    const char *name;
    bool isBitrateMode;
    bool isFloat;
    int valueMin;
    int valueMax;
};

const EncoderRCMode *getRCMode(ENUM_ENCODER encoder, const std::string& str);

// NVEnc --qvbr/--vbr-quality の品質上限 (H.264/HEVC: 51, AV1: 63)
int getRCModeValueMax(ENUM_ENCODER encoder, const EncoderRCMode* rcMode, VIDEO_STREAM_FORMAT format);

static std::vector<std::wstring> SplitOptions(const tstring& str);

EncoderOptionInfo ParseEncoderOption(ENUM_ENCODER encoder, const tstring& str);

// テスト用のレート制御モード名バッファ長 (終端文字を含む)
static constexpr size_t ENCODER_OPTION_FOR_TEST_RC_MODE_LENGTH = 16;

// DLL境界を越えて受け渡しするため、std::stringを含まない形にしたEncoderOptionInfo
struct EncoderOptionInfoForTest {
    int format;      // VIDEO_STREAM_FORMAT
    int deint;       // ENUM_ENCODER_DEINT
    int afsTimecode;
    int selectEvery;
    char rcMode[ENCODER_OPTION_FOR_TEST_RC_MODE_LENGTH];
    double rcModeValue[3];
    int parallel;
};

enum EncoderOptionForTestResult {
    ENCODER_OPTION_FOR_TEST_SUCCESS = 0,
    ENCODER_OPTION_FOR_TEST_INVALID_ARGUMENT = -1,
    ENCODER_OPTION_FOR_TEST_PARSE_FAILED = -2,
};

// ネイティブ単体テストからエンコーダオプション解析を呼び出すためのC ABI。
// 解析時に例外が発生した場合は ENCODER_OPTION_FOR_TEST_PARSE_FAILED を返す。
extern "C" AMATSUKAZE_API int ParseEncoderOptionForTest(
    int encoder, const tchar* options, EncoderOptionInfoForTest* result);

void PrintEncoderInfo(AMTContext& ctx, EncoderOptionInfo info);

