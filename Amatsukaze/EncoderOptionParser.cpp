/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include <limits>
#include "common.h"
#include "EncoderOptionParser.h"

static const EncoderRCMode RCMODES_X264_X265[] = {
    { _T("crf"),     false, true,  0, 51 },
    { _T("bitrate"), true,  false, 1, std::numeric_limits<int>::max() }
};

static const EncoderRCMode RCMODES_QSVENC[] = {
    { _T("icq"),    false, false, 1,  51 },
    { _T("la-icq"), false, false, 1,  51 },
    { _T("cqp"),    false, false, 0, 255 },
    { _T("vbr"),    true,  false, 1, std::numeric_limits<int>::max() },
    { _T("cbr"),    true,  false, 1, std::numeric_limits<int>::max() },
    { _T("avbr"),   true,  false, 1, std::numeric_limits<int>::max() },
    { _T("la"),     true,  false, 1, std::numeric_limits<int>::max() },
    { _T("la-hrd"), true,  false, 1, std::numeric_limits<int>::max() },
    { _T("vcm"),    true,  false, 1, std::numeric_limits<int>::max() },
    { _T("qvbr"),   true,  false, 1, std::numeric_limits<int>::max() }
};

static const EncoderRCMode RCMODES_NVENC[] = {
    { _T("qvbr"),  false, true,  1,  51  },
    { _T("cqp"),   false, false, 0, 255 },
    { _T("cbr"),   true,  false, 1, std::numeric_limits<int>::max() },
    { _T("cbrhq"), true,  false, 1, std::numeric_limits<int>::max() },
    { _T("vbr"),   true,  false, 1, std::numeric_limits<int>::max() },
    { _T("vbrhq"), true,  false, 1, std::numeric_limits<int>::max() }
};

static int QSVENC_DEFAULT_ICQ = 23;
static int NVENC_DEFAULT_QVBR = 25;

static std::vector<EncoderRCMode> encoderRCModes(ENUM_ENCODER encoder) {
    switch (encoder) {
    case ENCODER_QSVENC:
        return std::vector<EncoderRCMode>(std::begin(RCMODES_QSVENC), std::end(RCMODES_QSVENC));
    case ENCODER_NVENC:
        return std::vector<EncoderRCMode>(std::begin(RCMODES_NVENC), std::end(RCMODES_NVENC));
    default:
        return std::vector<EncoderRCMode>();
    }
}

const EncoderRCMode *getRCMode(ENUM_ENCODER encoder, const tstring& str) {
    if (str.empty() || str.length() == 0) return nullptr;

    const auto rcmodes = encoderRCModes(encoder);
    auto it = std::find_if(rcmodes.begin(), rcmodes.end(), [&str](const EncoderRCMode& mode) {
        return str == mode.name;
        });
    if (it != rcmodes.end()) {
        return &(*it);
    }
    return nullptr;
}

/* static */ std::vector<std::wstring> SplitOptions(const tstring& str) {
    std::wstring wstr = to_wstring(str);
    std::wregex re(L"(([^\" ]+)|\"([^\"]+)\") *");
    std::wsregex_iterator it(wstr.begin(), wstr.end(), re);
    std::wsregex_iterator end;
    std::vector<std::wstring> argv;
    for (; it != end; ++it) {
        if ((*it)[2].matched) {
            argv.push_back((*it)[2].str());
        } else if ((*it)[3].matched) {
            argv.push_back((*it)[3].str());
        }
    }
    return argv;
}

EncoderOptionInfo ParseEncoderOption(ENUM_ENCODER encoder, const tstring& str) {
    EncoderOptionInfo info = EncoderOptionInfo();

    if (encoder == ENCODER_X264) {
        info.format = VS_H264;
        return info;
    } else if (encoder == ENCODER_X265) {
        info.format = VS_H265;
        return info;
    } else if (encoder == ENCODER_SVTAV1) {
        info.format = VS_AV1;
        return info;
    }

    const auto rcmodes = encoderRCModes(encoder);

    auto argv = SplitOptions(str);
    int argc = (int)argv.size();

    //デフォルト値をセット
    info.format = VS_H264;
    switch (encoder) {
    case ENCODER_QSVENC:
        info.rcMode = rcmodes.front().name;
        info.rcModeValue[0] = QSVENC_DEFAULT_ICQ;
        break;
    case ENCODER_NVENC:
        info.rcMode = rcmodes.front().name;
        info.rcModeValue[0] = NVENC_DEFAULT_QVBR;
        break;
    default: break;
    }

    double qvbr_quality = -1.0;

    for (int i = 0; i < argc; ++i) {
        auto& arg = argv[i];
        auto& next = (i + 1 < argc) ? argv[i + 1] : L"";

        // argがrcmodesのnameに一致する場合、その要素を取得する
        if (auto it = std::find_if(rcmodes.begin(), rcmodes.end(), [&arg](const EncoderRCMode& mode) {
            return arg == (tstring(_T("--")) + mode.name);
            }); it != rcmodes.end()) {
            info.rcMode = it->name;
            if (tstring(it->name) == L"cqp") {
                std::wregex re3(L"([^:]+):([^:]+):([^:]+)");
                std::wregex re2(L"([^:]+):([^:]+)");
                std::wsmatch m;
                if (std::regex_match(next, m, re3)) {
                    info.rcModeValue[0] = std::stoi(m[1].str());
                    info.rcModeValue[1] = std::stoi(m[2].str());
                    info.rcModeValue[2] = std::stoi(m[3].str());
                } else if (std::regex_match(next, m, re2)) {
                    info.rcModeValue[0] = std::stoi(m[1].str());
                    info.rcModeValue[1] = std::stoi(m[2].str());
                    info.rcModeValue[2] = info.rcModeValue[1];
                } else {
                    info.rcModeValue[0] = std::stoi(next);
                    info.rcModeValue[1] = info.rcModeValue[0];
                    info.rcModeValue[2] = info.rcModeValue[1];
                }
            } else if (it->isFloat) {
                info.rcModeValue[0] = std::stod(next);
            } else {
                info.rcModeValue[0] = std::stoi(next);
            }
        } else if (arg == L"--vbr-quality") {
            qvbr_quality = std::stod(next);
        } else if (arg == L"--vpp-deinterlace") {
            if (next == L"normal" || next == L"adaptive") {
                info.deint = ENCODER_DEINT_30P;
            } else if (next == L"it") {
                info.deint = ENCODER_DEINT_24P;
            } else if (next == L"bob") {
                info.deint = ENCODER_DEINT_60P;
            }
        } else if (arg == L"--vpp-afs") {
            bool is24 = false;
            bool timecode = false;
            bool drop = false;
            std::wregex re(L"([^=]+)=([^,]+),?");
            std::wsregex_iterator it(next.begin(), next.end(), re);
            std::wsregex_iterator end;
            std::vector<std::wstring> argv;
            for (; it != end; ++it) {
                auto key = (*it)[1].str();
                auto val = (*it)[2].str();
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                if (key == L"24fps") {
                    is24 = (val == L"1" || val == L"true");
                } else if (key == L"drop") {
                    drop = (val == L"1" || val == L"true");
                } else if (key == L"timecode") {
                    timecode = (val == L"1" || val == L"true");
                } else if (key == L"preset") {
                    is24 = (val == L"24fps");
                    drop = (val == L"double" || val == L"anime" ||
                        val == L"cinema" || val == L"min_afterimg" || val == L"24fps");
                }
            }
            if (is24 && !drop) {
                THROW(ArgumentException,
                    "vpp-afsオプションに誤りがあります。24fps化する場合は間引き(drop)もonにする必要があります");
            }
            if (timecode) {
                info.deint = ENCODER_DEINT_VFR;
                info.afsTimecode = true;
            } else {
                info.deint = is24 ? ENCODER_DEINT_24P : ENCODER_DEINT_30P;
                info.afsTimecode = false;
            }
        } else if (arg == L"--vpp-select-every") {
            std::wregex re(L"([^=,]+)(=([^,]+))?,?");
            std::wsregex_iterator it(next.begin(), next.end(), re);
            std::wsregex_iterator end;
            std::vector<std::wstring> argv;
            for (; it != end; ++it) {
                auto key = (*it)[1].str();
                auto val = (*it)[3].str();
                if (val.length()) {
                    if (key == L"step") {
                        info.selectEvery = std::stoi(val);
                    }
                } else {
                    info.selectEvery = std::stoi(key);
                }
            }
        } else if (arg == L"-c" || arg == L"--codec") {
            if (next == L"h264") {
                info.format = VS_H264;
            } else if (next == L"hevc") {
                info.format = VS_H265;
            } else if (next == L"mpeg2") {
                info.format = VS_MPEG2;
            } else if (next == L"av1") {
                info.format = VS_AV1;
            } else {
                info.format = VS_UNKNOWN;
            }
        }
    }
    if (encoder == ENCODER_NVENC && info.rcModeValue[0] <= 0.0 && qvbr_quality >= 0.0) {
        info.rcMode = _T("qvbr");
        info.rcModeValue[0] = qvbr_quality;
    }

    return info;
}

void PrintEncoderInfo(AMTContext& ctx, EncoderOptionInfo info) {
    switch (info.deint) {
    case ENCODER_DEINT_NONE:
        ctx.info("エンコーダでのインタレ解除: なし");
        break;
    case ENCODER_DEINT_24P:
        ctx.info("エンコーダでのインタレ解除: 24fps化");
        break;
    case ENCODER_DEINT_30P:
        ctx.info("エンコーダでのインタレ解除: 30fps化");
        break;
    case ENCODER_DEINT_60P:
        ctx.info("エンコーダでのインタレ解除: 60fps化");
        break;
    case ENCODER_DEINT_VFR:
        ctx.info("エンコーダでのインタレ解除: VFR化");
        break;
    }
    if (info.selectEvery > 1) {
        ctx.infoF("エンコーダで%dフレームに1フレーム間引く", info.selectEvery);
    }
}
