/**
* Amtasukaze GenLogo CLI Entry point
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "rgy_osdep.h"
#include "rgy_tchar.h"
#include "rgy_util.h"
#include "rgy_codepage.h"

#include <array>
#include <cerrno>
#include <cstdarg>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#else
#include <iconv.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

static const char* kVersion = "dev";

enum : int {
    ERR_PARSE_BASE = -100,
    ERR_RUNTIME_BASE = 100,
    ERR_RUNTIME_EXE_PATH = 101,
    ERR_RUNTIME_LOAD_SYMBOL = 102,
    ERR_RUNTIME_INPUT_NOT_FOUND = 103,
    ERR_RUNTIME_CONTEXT_CREATE = 104,
    ERR_RUNTIME_SERVICE_ID = 105,
    ERR_RUNTIME_OUTPUT_NAME = 106,
    ERR_RUNTIME_OUTPUT_PLACE = 107,
    ERR_RUNTIME_NATIVE = 108,
};

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Options {
    tstring input;
    tstring output;
    std::optional<Rect> logoRange;
    int autoSearchFrames = 10000;
    int autoDivX = 5;
    int autoDivY = 5;
    int autoBlockSize = 32;
    int autoThreshold = 12;
    int autoMarginX = 6;
    int autoMarginY = 6;
    int autoThreads = 0;
    int logoGenThreshold = -1;
    int logoGenSamples = -1;
    bool aviutlLgd = false;
    tstring debugDir;
    tstring logoImagePath;
    bool showHelp = false;
    bool showVersion = false;
};

using LogoAnalyzeCallback = bool(*)(float progress, int nread, int total, int ngather);
using LogoAutoDetectCallback = bool(*)(int stage, float stageProgress, float progress, int nread, int total);

using InitAmatsukazeDLLFunc = void(*)();
using AMTContextCreateFunc = void*(*)();
using AMTContextDeleteFunc = void(*)(void*);
using AMTContextGetErrorFunc = const char*(*)(void*);
using TsInfoCreateFunc = void*(*)(void*);
using TsInfoDeleteFunc = void(*)(void*);
using TsInfoReadFileFunc = int(*)(void*, const TCHAR*);
using TsInfoHasServiceInfoFunc = int(*)(void*);
using TsInfoGetDayFunc = void(*)(void*, int*, int*, int*);
using TsInfoGetTimeFunc = void(*)(void*, int*, int*, int*);
using TsInfoGetNumProgramFunc = int(*)(void*);
using TsInfoGetProgramInfoFunc = void(*)(void*, int, int*, int*, int*, int*);
using TsInfoGetNumServiceFunc = int(*)(void*);
using TsInfoGetServiceIdFunc = int(*)(void*, int);
using TsInfoGetServiceNameFunc = const char*(*)(void*, int);
using ScanLogoFunc = int(*)(void*, const TCHAR*, int, const TCHAR*, const TCHAR*, const TCHAR*, int, int, int, int, int, int, LogoAnalyzeCallback);
using ScanLogoWithQualityValidationFunc = int(*)(void*, const TCHAR*, int, const TCHAR*, const TCHAR*, const TCHAR*, int, int, int, int, int, int, LogoAnalyzeCallback);
using AutoDetectLogoRectFunc = int(*)(void*, const TCHAR*, int, int, int, int, int, int, int, int, int,
    int*, int*, int*, int*, int*, int*,
    double*, double*, double*,
    int*, int*, int*, int*,
    int*, int*, int*, int*,
    int*, int*,
    const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*, const TCHAR*,
    int,
    LogoAutoDetectCallback);
using LogoFileCreateFunc = void*(*)(void*, const TCHAR*);
using LogoFileDeleteFunc = void(*)(void*);
using LogoFileSetServiceIdFunc = void(*)(void*, int);
using LogoFileSetNameFunc = void(*)(void*, const char*);
using LogoFileSaveFunc = int(*)(void*, const TCHAR*);
using LogoFileSaveImageJpegFunc = int(*)(void*, const TCHAR*, int, uint8_t);

struct NativeApi {
    InitAmatsukazeDLLFunc InitAmatsukazeDLL = nullptr;
    AMTContextCreateFunc AMTContext_Create = nullptr;
    AMTContextDeleteFunc ATMContext_Delete = nullptr;
    AMTContextGetErrorFunc AMTContext_GetError = nullptr;
    TsInfoCreateFunc TsInfo_Create = nullptr;
    TsInfoDeleteFunc TsInfo_Delete = nullptr;
    TsInfoReadFileFunc TsInfo_ReadFile = nullptr;
    TsInfoHasServiceInfoFunc TsInfo_HasServiceInfo = nullptr;
    TsInfoGetDayFunc TsInfo_GetDay = nullptr;
    TsInfoGetTimeFunc TsInfo_GetTime = nullptr;
    TsInfoGetNumProgramFunc TsInfo_GetNumProgram = nullptr;
    TsInfoGetProgramInfoFunc TsInfo_GetProgramInfo = nullptr;
    TsInfoGetNumServiceFunc TsInfo_GetNumService = nullptr;
    TsInfoGetServiceIdFunc TsInfo_GetServiceId = nullptr;
    TsInfoGetServiceNameFunc TsInfo_GetServiceName = nullptr;
    ScanLogoFunc ScanLogo = nullptr;
    ScanLogoWithQualityValidationFunc ScanLogoWithQualityValidation = nullptr;
    AutoDetectLogoRectFunc AutoDetectLogoRect = nullptr;
    LogoFileCreateFunc LogoFile_Create = nullptr;
    LogoFileDeleteFunc LogoFile_Delete = nullptr;
    LogoFileSetServiceIdFunc LogoFile_SetServiceId = nullptr;
    LogoFileSetNameFunc LogoFile_SetName = nullptr;
    LogoFileSaveFunc LogoFile_Save = nullptr;
    LogoFileSaveFunc LogoFile_SaveAviUtl = nullptr;
    LogoFileSaveImageJpegFunc LogoFile_SaveImageJpeg = nullptr;
};

struct AutoDetectProgressState {
    int lastStage = -1;
    int lastOverallBucket = -1;
    std::time_t lastReportTime = 0;
};

struct LogoGenProgressState {
    int lastPhase = -1;
    int lastProgressBucket = -1;
    std::time_t lastReportTime = 0;
};

AutoDetectProgressState* g_autoDetectProgressState = nullptr;
LogoGenProgressState* g_logoGenProgressState = nullptr;

std::tm GetLocalTime(std::time_t now);

tstring MakeLogTimestamp() {
    const auto now = std::time(nullptr);
    const auto localTime = GetLocalTime(now);
    TCHAR buffer[32] = {};
    _stprintf_s(buffer, _T("%04d-%02d-%02d %02d:%02d:%02d"),
        localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
        localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
    return tstring(buffer);
}

tstring VFormatLogMessage(const TCHAR* fmt, va_list args) {
    TCHAR buffer[4096] = {};
#if defined(_WIN32) || defined(_WIN64)
    va_list argsCopy;
    va_copy(argsCopy, args);
    vswprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), fmt, argsCopy);
    va_end(argsCopy);
#else
    va_list argsCopy;
    va_copy(argsCopy, args);
    vsnprintf(buffer, sizeof(buffer), fmt, argsCopy);
    va_end(argsCopy);
#endif
    return tstring(buffer);
}

void PrintCliInfo(const TCHAR* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const tstring message = VFormatLogMessage(fmt, args);
    va_end(args);
    _ftprintf(stdout, _T("%s [GenLogoCLI] %s\n"), MakeLogTimestamp().c_str(), message.c_str());
    fflush(stdout);
}

float ClampPercent(float value) {
    return std::max(0.0f, std::min(100.0f, value));
}

const TCHAR* GetAutoDetectStageName(int stage) {
    switch (stage) {
    case 1: return _T("初期フレーム走査");
    case 2: return _T("仮推定とFrameGate準備");
    case 3: return _T("最終推定と矩形確定");
    case 4: return _T("完了");
    default: return _T("待機中");
    }
}

const TCHAR* GetLogoGenPhaseName(float progressPercent) {
    if (progressPercent >= 99.9f) {
        return _T("完了");
    }
    if (progressPercent >= 75.0f) {
        return _T("ロゴ再構築(2回目)");
    }
    if (progressPercent >= 50.0f) {
        return _T("ロゴ再構築(1回目)");
    }
    return _T("初期ロゴ生成");
}

float NormalizeLogoGenProgress(float progress, int nread, int total) {
    if (total > 0 && nread >= total && progress <= 1.5f) {
        return 100.0f;
    }
    return ClampPercent(progress);
}

bool ReportAnalyzeProgress(float progress, int nread, int total, int) {
    if (g_logoGenProgressState == nullptr) {
        return true;
    }

    auto& state = *g_logoGenProgressState;
    const float progressPercent = NormalizeLogoGenProgress(progress, nread, total);
    const int progressBucket = (progressPercent >= 100.0f) ? 10 : (int)(progressPercent / 10.0f);
    const int phase =
        (progressPercent >= 99.9f) ? 4 :
        (progressPercent >= 75.0f) ? 3 :
        (progressPercent >= 50.0f) ? 2 : 1;
    const auto now = std::time(nullptr);

    if (phase != state.lastPhase) {
        if (total > 0) {
            PrintCliInfo(_T("logo generate phase: %s progress=%.1f%% read=%d/%d"),
                GetLogoGenPhaseName(progressPercent), progressPercent, nread, total);
        } else {
            PrintCliInfo(_T("logo generate phase: %s progress=%.1f%% read=%d"),
                GetLogoGenPhaseName(progressPercent), progressPercent, nread);
        }
        state.lastPhase = phase;
        state.lastProgressBucket = progressBucket;
        state.lastReportTime = now;
        return true;
    }

    if (progressBucket > state.lastProgressBucket || std::difftime(now, state.lastReportTime) >= 5.0) {
        if (total > 0) {
            PrintCliInfo(_T("logo generate progress: %s progress=%.1f%% read=%d/%d"),
                GetLogoGenPhaseName(progressPercent), progressPercent, nread, total);
        } else {
            PrintCliInfo(_T("logo generate progress: %s progress=%.1f%% read=%d"),
                GetLogoGenPhaseName(progressPercent), progressPercent, nread);
        }
        state.lastProgressBucket = progressBucket;
        state.lastReportTime = now;
    }
    return true;
}

bool ReportAutoDetectProgress(int stage, float stageProgress, float progress, int nread, int total) {
    if (g_autoDetectProgressState == nullptr) {
        return true;
    }

    auto& state = *g_autoDetectProgressState;
    const float overallPercent = ClampPercent(progress * 100.0f);
    const float stagePercent = ClampPercent(stageProgress * 100.0f);
    const int overallBucket = (overallPercent >= 100.0f) ? 10 : (int)(overallPercent / 10.0f);
    const auto now = std::time(nullptr);

    if (stage != state.lastStage) {
        PrintCliInfo(_T("auto detect phase: %s overall=%.1f%% stage=%.1f%% read=%d/%d"),
            GetAutoDetectStageName(stage), overallPercent, stagePercent, nread, total);
        state.lastStage = stage;
        state.lastOverallBucket = overallBucket;
        state.lastReportTime = now;
        return true;
    }

    if (overallBucket > state.lastOverallBucket || std::difftime(now, state.lastReportTime) >= 5.0) {
        PrintCliInfo(_T("auto detect progress: %s overall=%.1f%% stage=%.1f%% read=%d/%d"),
            GetAutoDetectStageName(stage), overallPercent, stagePercent, nread, total);
        state.lastOverallBucket = overallBucket;
        state.lastReportTime = now;
    }
    return true;
}

void PrintUsage() {
    _ftprintf(stdout,
        _T("AmatsukazeGenLogo - TSから局ロゴ用lgdを生成します\n")
        _T("\n")
        _T("Usage:\n")
        _T("  AmatsukazeGenLogo -i <input.ts> -o <output.lgd> [options]\n")
        _T("\n")
        _T("Main options:\n")
        _T("  -i, --input <path>                         入力TSファイル\n")
        _T("  -o, --output <path>                        出力lgdファイル\n")
        _T("\n")
        _T("Logo rect options:\n")
        _T("      --logo-range <x>,<y>,<w>,<h>          ロゴ枠を手動指定\n")
        _T("      --auto-logo-detect-search-frames <n>  自動ロゴ枠検出の検索フレーム数 [10000]\n")
        _T("      --auto-logo-detect-div-x <n>          自動ロゴ枠検出の分割数X [5]\n")
        _T("      --auto-logo-detect-div-y <n>          自動ロゴ枠検出の分割数Y [5]\n")
        _T("      --auto-logo-detect-block-size <n>     自動ロゴ枠検出のブロックサイズ [32]\n")
        _T("      --auto-logo-detect-threshold <n>      自動ロゴ枠検出の閾値 [12]\n")
        _T("      --auto-logo-detect-margin-x <n>       自動ロゴ枠検出のマージンX [6]\n")
        _T("      --auto-logo-detect-margin-y <n>       自動ロゴ枠検出のマージンY [6]\n")
        _T("      --auto-logo-detect-threads <n>        自動ロゴ枠検出スレッド数 [0=max(1,min(論理コア数-2,16))]\n")
        _T("\n")
        _T("Logo generate options:\n")
        _T("      --logo-gen-threshold <n>              ロゴ生成の閾値 [auto-detect-thresholdと同値]\n")
        _T("      --logo-gen-samples <n>                ロゴ生成の最大サンプル数 [search-framesと同値]\n")
        _T("\n")
        _T("Other options:\n")
        _T("      --aviutl-lgd                          AviUtl向けlgdを保存\n")
        _T("      --output-logo-image <path>            ロゴ画像をJPEGで保存\n")
        _T("      --debug-dir <path>                    自動ロゴ枠検出デバッグ画像出力先\n")
        _T("  -h, --help, /?                            このヘルプを表示\n")
        _T("      --version                             バージョンを表示\n"));
}

void PrintVersion() {
#if defined(_WIN32) || defined(_WIN64)
    _ftprintf(stdout, _T("AmatsukazeGenLogo %S\n"), kVersion);
#else
    _ftprintf(stdout, _T("AmatsukazeGenLogo %s\n"), kVersion);
#endif
}

bool ParseInt(const tstring& value, const TCHAR* optionName, int& outValue, int& errCode) {
    try {
        size_t pos = 0;
        const int result = std::stoi(value, &pos, 10);
        if (pos != value.size()) {
            _ftprintf(stderr, _T("AmatsukazeGenLogo error: %s の値が不正です\n"), optionName);
            errCode = ERR_PARSE_BASE - 2;
            return false;
        }
        outValue = result;
        return true;
    } catch (...) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: %s の値が不正です\n"), optionName);
        errCode = ERR_PARSE_BASE - 2;
        return false;
    }
}

bool ParseRect(const tstring& value, Rect& outRect, int& errCode) {
    std::array<int, 4> values = {};
    size_t start = 0;
    for (int i = 0; i < 4; i++) {
        size_t end = value.find(_T(','), start);
        if (i == 3) {
            end = tstring::npos;
        } else if (end == tstring::npos) {
            _ftprintf(stderr, _T("AmatsukazeGenLogo error: --logo-range の形式は x,y,w,h です\n"));
            errCode = ERR_PARSE_BASE - 3;
            return false;
        }
        const tstring part = value.substr(start, (end == tstring::npos) ? tstring::npos : (end - start));
        if (!ParseInt(part, _T("--logo-range"), values[i], errCode)) {
            return false;
        }
        start = (end == tstring::npos) ? end : end + 1;
    }
    if (values[2] <= 0 || values[3] <= 0) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: --logo-range の幅と高さは1以上で指定してください\n"));
        errCode = ERR_PARSE_BASE - 4;
        return false;
    }
    outRect = Rect{ values[0], values[1], values[2], values[3] };
    return true;
}

int ParseArgs(int argc, const TCHAR* argv[], Options& opt) {
    for (int i = 1; i < argc; i++) {
        const tstring key = argv[i];
        auto requireValue = [&](const TCHAR* optionName, tstring& outValue) -> bool {
            if (i + 1 >= argc) {
                _ftprintf(stderr, _T("AmatsukazeGenLogo error: %s の値が不足しています\n"), optionName);
                return false;
            }
            outValue = argv[++i];
            return true;
        };

        if (key == _T("-i") || key == _T("--input")) {
            if (!requireValue(key.c_str(), opt.input)) return ERR_PARSE_BASE - 1;
        } else if (key == _T("-o") || key == _T("--output")) {
            if (!requireValue(key.c_str(), opt.output)) return ERR_PARSE_BASE - 1;
        } else if (key == _T("--logo-range")) {
            tstring value;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            Rect rect{};
            int errCode = 0;
            if (!ParseRect(value, rect, errCode)) return errCode;
            opt.logoRange = rect;
        } else if (key == _T("--auto-logo-detect-search-frames")) {
            tstring value; int errCode = 0;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            if (!ParseInt(value, key.c_str(), opt.autoSearchFrames, errCode)) return errCode;
        } else if (key == _T("--auto-logo-detect-div-x")) {
            tstring value; int errCode = 0;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            if (!ParseInt(value, key.c_str(), opt.autoDivX, errCode)) return errCode;
        } else if (key == _T("--auto-logo-detect-div-y")) {
            tstring value; int errCode = 0;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            if (!ParseInt(value, key.c_str(), opt.autoDivY, errCode)) return errCode;
        } else if (key == _T("--auto-logo-detect-block-size")) {
            tstring value; int errCode = 0;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            if (!ParseInt(value, key.c_str(), opt.autoBlockSize, errCode)) return errCode;
        } else if (key == _T("--auto-logo-detect-threshold")) {
            tstring value; int errCode = 0;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            if (!ParseInt(value, key.c_str(), opt.autoThreshold, errCode)) return errCode;
        } else if (key == _T("--auto-logo-detect-margin-x")) {
            tstring value; int errCode = 0;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            if (!ParseInt(value, key.c_str(), opt.autoMarginX, errCode)) return errCode;
        } else if (key == _T("--auto-logo-detect-margin-y")) {
            tstring value; int errCode = 0;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            if (!ParseInt(value, key.c_str(), opt.autoMarginY, errCode)) return errCode;
        } else if (key == _T("--auto-logo-detect-threads")) {
            tstring value; int errCode = 0;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            if (!ParseInt(value, key.c_str(), opt.autoThreads, errCode)) return errCode;
        } else if (key == _T("--logo-gen-threshold")) {
            tstring value; int errCode = 0;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            if (!ParseInt(value, key.c_str(), opt.logoGenThreshold, errCode)) return errCode;
        } else if (key == _T("--logo-gen-samples")) {
            tstring value; int errCode = 0;
            if (!requireValue(key.c_str(), value)) return ERR_PARSE_BASE - 1;
            if (!ParseInt(value, key.c_str(), opt.logoGenSamples, errCode)) return errCode;
        } else if (key == _T("--aviutl-lgd")) {
            opt.aviutlLgd = true;
        } else if (key == _T("--output-logo-image")) {
            if (!requireValue(key.c_str(), opt.logoImagePath)) return ERR_PARSE_BASE - 1;
        } else if (key == _T("--debug-dir")) {
            if (!requireValue(key.c_str(), opt.debugDir)) return ERR_PARSE_BASE - 1;
        } else if (key == _T("-h") || key == _T("--help") || key == _T("/?")) {
            opt.showHelp = true;
        } else if (key == _T("--version")) {
            opt.showVersion = true;
        } else {
            _ftprintf(stderr, _T("AmatsukazeGenLogo error: 不明なオプションです: %s\n"), key.c_str());
            return ERR_PARSE_BASE - 5;
        }
    }

    if (opt.showHelp || opt.showVersion) {
        return 0;
    }
    if (opt.input.empty()) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: --input を指定してください\n"));
        return ERR_PARSE_BASE - 6;
    }
    if (opt.output.empty()) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: --output を指定してください\n"));
        return ERR_PARSE_BASE - 6;
    }
    if (opt.autoDivX <= 0 || opt.autoDivY <= 0) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: div-x/div-y は1以上で指定してください\n"));
        return ERR_PARSE_BASE - 7;
    }
    if (opt.autoSearchFrames < 100) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: search-frames は100以上で指定してください\n"));
        return ERR_PARSE_BASE - 7;
    }
    if (opt.autoBlockSize < 4) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: block-size は4以上で指定してください\n"));
        return ERR_PARSE_BASE - 7;
    }
    if (opt.autoThreshold < 1) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: auto-detect-threshold は1以上で指定してください\n"));
        return ERR_PARSE_BASE - 7;
    }
    if (opt.autoMarginX < 0 || opt.autoMarginY < 0) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: margin-x/margin-y は0以上で指定してください\n"));
        return ERR_PARSE_BASE - 7;
    }
    if (opt.autoThreads < 0) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: auto-logo-detect-threads は0以上で指定してください\n"));
        return ERR_PARSE_BASE - 7;
    }
    if (opt.logoGenThreshold == -1) {
        opt.logoGenThreshold = opt.autoThreshold;
    }
    if (opt.logoGenSamples == -1) {
        opt.logoGenSamples = opt.autoSearchFrames;
    }
    if (opt.logoGenThreshold < 1) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: logo-gen-threshold は1以上で指定してください\n"));
        return ERR_PARSE_BASE - 7;
    }
    if (opt.logoGenSamples < 1) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: logo-gen-samples は1以上で指定してください\n"));
        return ERR_PARSE_BASE - 7;
    }
    return 0;
}

int GetExecutablePath(fs::path& exePath) {
#if defined(_WIN32) || defined(_WIN64)
    std::wstring buffer(32768, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: GetModuleFileNameW failed\n"));
        return ERR_RUNTIME_EXE_PATH;
    }
    buffer.resize(len);
    exePath = fs::path(buffer);
    return 0;
#else
    std::vector<char> buffer(32768, '\0');
    ssize_t len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len <= 0) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: readlink(/proc/self/exe) failed\n"));
        return ERR_RUNTIME_EXE_PATH;
    }
    buffer[(size_t)len] = '\0';
    exePath = fs::path(buffer.data());
    return 0;
#endif
}

template<typename T>
bool LoadSymbol(HMODULE module, const char* name, T& symbol) {
    symbol = reinterpret_cast<T>(RGY_GET_PROC_ADDRESS(module, name));
    if (symbol == nullptr) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: Failed to load symbol: %hs\n"), name);
        return false;
    }
    return true;
}

int LoadNativeApi(HMODULE module, NativeApi& api) {
    if (!LoadSymbol(module, "InitAmatsukazeDLL", api.InitAmatsukazeDLL)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "AMTContext_Create", api.AMTContext_Create)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "ATMContext_Delete", api.ATMContext_Delete)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "AMTContext_GetError", api.AMTContext_GetError)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_Create", api.TsInfo_Create)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_Delete", api.TsInfo_Delete)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_ReadFile", api.TsInfo_ReadFile)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_HasServiceInfo", api.TsInfo_HasServiceInfo)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_GetDay", api.TsInfo_GetDay)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_GetTime", api.TsInfo_GetTime)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_GetNumProgram", api.TsInfo_GetNumProgram)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_GetProgramInfo", api.TsInfo_GetProgramInfo)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_GetNumService", api.TsInfo_GetNumService)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_GetServiceId", api.TsInfo_GetServiceId)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "TsInfo_GetServiceName", api.TsInfo_GetServiceName)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "ScanLogo", api.ScanLogo)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "ScanLogoWithQualityValidation", api.ScanLogoWithQualityValidation)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "AutoDetectLogoRect", api.AutoDetectLogoRect)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "LogoFile_Create", api.LogoFile_Create)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "LogoFile_Delete", api.LogoFile_Delete)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "LogoFile_SetServiceId", api.LogoFile_SetServiceId)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "LogoFile_SetName", api.LogoFile_SetName)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "LogoFile_Save", api.LogoFile_Save)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "LogoFile_SaveAviUtl", api.LogoFile_SaveAviUtl)) return ERR_RUNTIME_LOAD_SYMBOL;
    if (!LoadSymbol(module, "LogoFile_SaveImageJpeg", api.LogoFile_SaveImageJpeg)) return ERR_RUNTIME_LOAD_SYMBOL;
    return 0;
}

std::string SafeGetError(const NativeApi& api, void* ctx) {
    const char* err = (ctx && api.AMTContext_GetError) ? api.AMTContext_GetError(ctx) : nullptr;
    return (err != nullptr) ? std::string(err) : std::string("unknown native error");
}

std::string TruncateNameBytes(std::string name) {
    constexpr size_t kMaxNameBytes = 254;
    if (name.size() > kMaxNameBytes) {
        name.resize(kMaxNameBytes);
    }
    return name;
}

std::string Utf8ToCp932(const std::string& utf8) {
    if (utf8.empty()) {
        return std::string();
    }
#if defined(_WIN32) || defined(_WIN64)
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) {
        return utf8;
    }
    std::wstring wide((size_t)wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), wideLen);
    int sjisLen = WideCharToMultiByte(932, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sjisLen <= 0) {
        return utf8;
    }
    std::string sjis((size_t)sjisLen - 1, '\0');
    WideCharToMultiByte(932, 0, wide.c_str(), -1, sjis.data(), sjisLen, nullptr, nullptr);
    return sjis;
#else
    const char* encodings[] = { "CP932", "SHIFT-JIS", "SJIS" };
    for (auto encoding : encodings) {
        iconv_t cd = iconv_open(encoding, "UTF-8");
        if (cd == (iconv_t)-1) {
            continue;
        }
        size_t inBytes = utf8.size();
        size_t outBytes = utf8.size() * 4 + 16;
        std::vector<char> out(outBytes, '\0');
        char* inBuf = const_cast<char*>(utf8.data());
        char* outBuf = out.data();
        size_t outRemain = out.size();
        if (iconv(cd, &inBuf, &inBytes, &outBuf, &outRemain) != (size_t)-1) {
            iconv_close(cd);
            return std::string(out.data(), out.size() - outRemain);
        }
        iconv_close(cd);
    }
    return utf8;
#endif
}

tstring JoinDateString(int year, int month, int day) {
    TCHAR buffer[32] = {};
    _stprintf_s(buffer, _T("%04d-%02d-%02d"), year, month, day);
    return tstring(buffer);
}

std::string MakeLogoNameCp932(const NativeApi& api, void* tsInfo, int serviceId) {
    if (api.TsInfo_HasServiceInfo(tsInfo) == 0) {
        return Utf8ToCp932("情報なし");
    }

    int year = 0, month = 0, day = 0;
    int hour = 0, minute = 0, second = 0;
    api.TsInfo_GetDay(tsInfo, &year, &month, &day);
    api.TsInfo_GetTime(tsInfo, &hour, &minute, &second);

    std::string serviceNameUtf8;
    const int numServices = api.TsInfo_GetNumService(tsInfo);
    for (int i = 0; i < numServices; i++) {
        if (api.TsInfo_GetServiceId(tsInfo, i) == serviceId) {
            const char* name = api.TsInfo_GetServiceName(tsInfo, i);
            if (name != nullptr) {
                serviceNameUtf8 = name;
            }
            break;
        }
    }
    if (serviceNameUtf8.empty()) {
        return Utf8ToCp932("情報なし");
    }

    std::string dateUtf8;
    {
        const tstring date = JoinDateString(year, month, day);
#if defined(_WIN32) || defined(_WIN64)
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, date.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string tmp((size_t)utf8Len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, date.c_str(), -1, tmp.data(), utf8Len, nullptr, nullptr);
        dateUtf8 = tmp;
#else
        dateUtf8 = date;
#endif
    }
    return Utf8ToCp932(serviceNameUtf8 + "(" + dateUtf8 + ")");
}

Rect AlignRect(const Rect& rect) {
    const int x = (rect.x / 2) * 2;
    const int y = (rect.y / 2) * 2;
    const int w = ((rect.w + 1) / 2) * 2;
    const int h = ((rect.h + 1) / 2) * 2;
    return Rect{ x, y, w, h };
}

int GetProcessIdValue() {
#if defined(_WIN32) || defined(_WIN64)
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

tstring IntToTString(int value) {
    TCHAR buffer[32] = {};
    _stprintf_s(buffer, _T("%d"), value);
    return tstring(buffer);
}

struct TempPaths {
    tstring workFile;
    tstring tempLogo;
    tstring finalTemp;
};

TempPaths MakeTempPaths(const tstring& outputPath) {
    const fs::path output(outputPath);
    const fs::path parent = output.has_parent_path() ? output.parent_path() : fs::current_path();
    const tstring stem = output.stem().native();
    const tstring tag = stem + _T(".genlogo.") + IntToTString(GetProcessIdValue());
    const fs::path workFile = parent / (tag + _T(".work.dat"));
    const fs::path tempLogo = parent / (tag + _T(".tmp.lgd"));
    const fs::path finalTemp = parent / (tag + _T(".out.lgd"));
    return TempPaths{ workFile.native(), tempLogo.native(), finalTemp.native() };
}

struct DebugPaths {
    tstring score;
    tstring binary;
    tstring ccl;
    tstring count;
    tstring a;
    tstring b;
    tstring alpha;
    tstring logoY;
    tstring consistency;
    tstring fgVar;
    tstring bgVar;
    tstring transition;
    tstring keepRate;
    tstring accepted;
};

DebugPaths MakeDebugPaths(const tstring& dir) {
    const fs::path base(dir);
    return DebugPaths{
        (base / _T("score.bmp")).native(),
        (base / _T("binary.bmp")).native(),
        (base / _T("ccl.bmp")).native(),
        (base / _T("count.bmp")).native(),
        (base / _T("a.bmp")).native(),
        (base / _T("b.bmp")).native(),
        (base / _T("alpha.bmp")).native(),
        (base / _T("logoy.bmp")).native(),
        (base / _T("consistency.bmp")).native(),
        (base / _T("fgvar.bmp")).native(),
        (base / _T("bgvar.bmp")).native(),
        (base / _T("transition.bmp")).native(),
        (base / _T("keeprate.bmp")).native(),
        (base / _T("accepted.bmp")).native(),
    };
}

int ResolveServiceId(const NativeApi& api, void* tsInfo, int& serviceId) {
    std::vector<int> videoPrograms;
    const int numPrograms = api.TsInfo_GetNumProgram(tsInfo);
    for (int i = 0; i < numPrograms; i++) {
        int programId = 0;
        int hasVideo = 0;
        int videoPid = 0;
        int numContent = 0;
        api.TsInfo_GetProgramInfo(tsInfo, i, &programId, &hasVideo, &videoPid, &numContent);
        if (hasVideo != 0 && programId > 0) {
            videoPrograms.push_back(programId);
        }
    }
    if (videoPrograms.size() == 1) {
        serviceId = videoPrograms[0];
        return 0;
    }

    const int numServices = api.TsInfo_GetNumService(tsInfo);
    if (numServices == 1) {
        serviceId = api.TsInfo_GetServiceId(tsInfo, 0);
        return 0;
    }
    _ftprintf(stderr, _T("AmatsukazeGenLogo error: service id を一意に解決できません\n"));
    return ERR_RUNTIME_SERVICE_ID;
}

void RemoveIfExists(const tstring& path) {
    if (!path.empty()) {
        std::error_code ec;
        fs::remove(fs::path(path), ec);
    }
}

std::tm GetLocalTime(std::time_t now) {
    std::tm localTime = {};
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif
    return localTime;
}

tstring MakeOutputTimestamp() {
    const auto now = std::time(nullptr);
    const auto localTime = GetLocalTime(now);
    TCHAR buffer[32] = {};
    _stprintf_s(buffer, _T("%04d%02d%02d_%02d%02d%02d"),
        localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
        localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
    return tstring(buffer);
}

int ResolveFinalOutputPath(const tstring& outputPath, fs::path& finalPath) {
    const fs::path output(outputPath);
    if (!fs::exists(output)) {
        finalPath = output;
        return 0;
    }

    const fs::path parent = output.has_parent_path() ? output.parent_path() : fs::current_path();
    const tstring stem = output.stem().native();
    const tstring ext = output.has_extension() ? output.extension().native() : tstring(_T(".lgd"));
    const tstring timestamp = MakeOutputTimestamp();
    for (int attempt = 0; attempt < 100; attempt++) {
        tstring filename = stem + _T("-") + timestamp;
        if (attempt > 0) {
            filename += _T("-") + IntToTString(attempt);
        }
        filename += ext;
        const fs::path candidate = parent / filename;
        if (!fs::exists(candidate)) {
            _ftprintf(stderr,
                _T("出力先に既存ファイルがあるため、別名で保存します: %s -> %s\n"),
                output.c_str(), candidate.c_str());
            finalPath = candidate;
            return 0;
        }
    }
    _ftprintf(stderr, _T("AmatsukazeGenLogo error: 退避用の出力ファイル名を決定できません\n"));
    return ERR_RUNTIME_OUTPUT_NAME;
}

int FinalizeOutput(const tstring& tempPath, const tstring& outputPath, fs::path& finalPath) {
    int ret = ResolveFinalOutputPath(outputPath, finalPath);
    if (ret != 0) {
        return ret;
    }
    std::error_code ec;
    fs::rename(fs::path(tempPath), finalPath, ec);
    if (!ec) {
        return 0;
    }
    fs::copy_file(fs::path(tempPath), finalPath, fs::copy_options::none, ec);
    if (ec) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: 出力ファイルの配置に失敗しました\n"));
        return ERR_RUNTIME_OUTPUT_PLACE;
    }
    fs::remove(fs::path(tempPath), ec);
    return 0;
}

int Run(const NativeApi& api, const Options& opt) {
    struct ContextDeleter {
        const NativeApi* apiPtr = nullptr;
        void operator()(void* p) const {
            if (p != nullptr && apiPtr != nullptr) {
                apiPtr->ATMContext_Delete(p);
            }
        }
    };
    struct TsInfoDeleter {
        const NativeApi* apiPtr = nullptr;
        void operator()(void* p) const {
            if (p != nullptr && apiPtr != nullptr) {
                apiPtr->TsInfo_Delete(p);
            }
        }
    };
    struct LogoDeleter {
        const NativeApi* apiPtr = nullptr;
        void operator()(void* p) const {
            if (p != nullptr && apiPtr != nullptr) {
                apiPtr->LogoFile_Delete(p);
            }
        }
    };
    struct ProgressGuard {
        ~ProgressGuard() {
            g_autoDetectProgressState = nullptr;
            g_logoGenProgressState = nullptr;
        }
    };

    const fs::path inputPath(opt.input);
    if (!fs::exists(inputPath)) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: 入力ファイルが存在しません\n"));
        return ERR_RUNTIME_INPUT_NOT_FOUND;
    }
    if (fs::path(opt.output).has_parent_path()) {
        fs::create_directories(fs::path(opt.output).parent_path());
    }
    const bool detailedDebug = !opt.debugDir.empty();
    DebugPaths debugPaths{};
    if (detailedDebug) {
        fs::create_directories(fs::path(opt.debugDir));
        debugPaths = MakeDebugPaths(opt.debugDir);
    }

    const TempPaths tempPaths = MakeTempPaths(opt.output);
    RemoveIfExists(tempPaths.workFile);
    RemoveIfExists(tempPaths.tempLogo);
    RemoveIfExists(tempPaths.finalTemp);

    std::unique_ptr<void, ContextDeleter> ctx(api.AMTContext_Create(), ContextDeleter{ &api });
    if (!ctx) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: AMTContext_Create に失敗しました\n"));
        return ERR_RUNTIME_CONTEXT_CREATE;
    }
    ProgressGuard progressGuard{};

    std::unique_ptr<void, TsInfoDeleter> tsInfo(api.TsInfo_Create(ctx.get()), TsInfoDeleter{ &api });
    if (!tsInfo) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: %hs\n"), SafeGetError(api, ctx.get()).c_str());
        return ERR_RUNTIME_NATIVE;
    }
    if (api.TsInfo_ReadFile(tsInfo.get(), opt.input.c_str()) == 0) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: %hs\n"), SafeGetError(api, ctx.get()).c_str());
        return ERR_RUNTIME_NATIVE;
    }

    int serviceId = 0;
    int ret = ResolveServiceId(api, tsInfo.get(), serviceId);
    if (ret != 0) {
        return ret;
    }
    PrintCliInfo(_T("start: input=%s output=%s serviceId=%d aviutl=%d debugDir=%s"),
        opt.input.c_str(), opt.output.c_str(), serviceId, (int)opt.aviutlLgd,
        detailedDebug ? opt.debugDir.c_str() : _T("<none>"));
    const bool autoDetectedRect = !opt.logoRange.has_value();
    Rect rect = opt.logoRange.value_or(Rect{});
    if (autoDetectedRect) {
        int x = 0, y = 0, w = 0, h = 0;
        int rectDetectFail = 0;
        int logoAnalyzeFail = 0;
        double pass1ScoreMax = 0.0;
        double pass2ScoreMax = 0.0;
        double finalScoreBeforeRescueMax = 0.0;
        int pass2Entered = 0;
        int pass2PrepareSucceeded = 0;
        int pass2CollectSucceeded = 0;
        int pass2RescueFallbackApplied = 0;
        int pass2FailBeforeClear = 0;
        int pass2FrameMaskNonZero = 0;
        int pass2AcceptedFrames = 0;
        int pass2SkippedFrames = 0;
        int frameGateRetryAttemptCount = 0;
        int frameGateRetrySuccessAttempt = 0;

        PrintCliInfo(_T("auto detect params: div=%dx%d searchFrames=%d blockSize=%d threshold=%d margin=(%d,%d) threadN=%d detailedDebug=%d"),
            opt.autoDivX, opt.autoDivY, opt.autoSearchFrames, opt.autoBlockSize, opt.autoThreshold,
            opt.autoMarginX, opt.autoMarginY, opt.autoThreads, (int)detailedDebug);
        AutoDetectProgressState autoDetectProgressState{};
        g_autoDetectProgressState = &autoDetectProgressState;

        if (api.AutoDetectLogoRect(
            ctx.get(), opt.input.c_str(), serviceId,
            opt.autoDivX, opt.autoDivY, opt.autoSearchFrames, opt.autoBlockSize, opt.autoThreshold,
            opt.autoMarginX, opt.autoMarginY, opt.autoThreads,
            &x, &y, &w, &h, &rectDetectFail, &logoAnalyzeFail,
            &pass1ScoreMax, &pass2ScoreMax, &finalScoreBeforeRescueMax,
            &pass2Entered, &pass2PrepareSucceeded, &pass2CollectSucceeded, &pass2RescueFallbackApplied,
            &pass2FailBeforeClear, &pass2FrameMaskNonZero, &pass2AcceptedFrames, &pass2SkippedFrames,
            &frameGateRetryAttemptCount, &frameGateRetrySuccessAttempt,
            detailedDebug ? debugPaths.score.c_str() : nullptr,
            detailedDebug ? debugPaths.binary.c_str() : nullptr,
            detailedDebug ? debugPaths.ccl.c_str() : nullptr,
            detailedDebug ? debugPaths.count.c_str() : nullptr,
            detailedDebug ? debugPaths.a.c_str() : nullptr,
            detailedDebug ? debugPaths.b.c_str() : nullptr,
            detailedDebug ? debugPaths.alpha.c_str() : nullptr,
            detailedDebug ? debugPaths.logoY.c_str() : nullptr,
            detailedDebug ? debugPaths.consistency.c_str() : nullptr,
            detailedDebug ? debugPaths.fgVar.c_str() : nullptr,
            detailedDebug ? debugPaths.bgVar.c_str() : nullptr,
            detailedDebug ? debugPaths.transition.c_str() : nullptr,
            detailedDebug ? debugPaths.keepRate.c_str() : nullptr,
            detailedDebug ? debugPaths.accepted.c_str() : nullptr,
            detailedDebug ? 1 : 0,
            ReportAutoDetectProgress) == 0) {
            _ftprintf(stderr, _T("AmatsukazeGenLogo error: %hs\n"), SafeGetError(api, ctx.get()).c_str());
            g_autoDetectProgressState = nullptr;
            return ERR_RUNTIME_NATIVE;
        }
        g_autoDetectProgressState = nullptr;
        PrintCliInfo(_T("auto detect result: rect=(%d,%d,%d,%d) rectDetectFail=%d logoAnalyzeFail=%d pass1ScoreMax=%.4f pass2ScoreMax=%.4f finalScoreBeforeRescueMax=%.4f pass2={entered=%d prepare=%d collect=%d fallback=%d acceptedFrames=%d skippedFrames=%d retryAttempt=%d retrySuccess=%d}"),
            x, y, w, h, rectDetectFail, logoAnalyzeFail,
            pass1ScoreMax, pass2ScoreMax, finalScoreBeforeRescueMax,
            pass2Entered, pass2PrepareSucceeded, pass2CollectSucceeded, pass2RescueFallbackApplied,
            pass2AcceptedFrames, pass2SkippedFrames, frameGateRetryAttemptCount, frameGateRetrySuccessAttempt);
        rect = Rect{ x, y, w, h };
    } else {
        PrintCliInfo(_T("manual logo rect: rect=(%d,%d,%d,%d)"), rect.x, rect.y, rect.w, rect.h);
    }

    const Rect aligned = AlignRect(rect);
    if (aligned.w <= 0 || aligned.h <= 0) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: ロゴ枠が不正です\n"));
        return ERR_RUNTIME_NATIVE;
    }
    PrintCliInfo(_T("logo generate params: rect=(%d,%d,%d,%d) aligned=(%d,%d,%d,%d) threshold=%d maxFrames=%d"),
        rect.x, rect.y, rect.w, rect.h,
        aligned.x, aligned.y, aligned.w, aligned.h,
        opt.logoGenThreshold, opt.logoGenSamples);

    LogoGenProgressState logoGenProgressState{};
    g_logoGenProgressState = &logoGenProgressState;

    auto scanLogo = autoDetectedRect ? api.ScanLogoWithQualityValidation : api.ScanLogo;
    if (scanLogo(
        ctx.get(), opt.input.c_str(), serviceId,
        tempPaths.workFile.c_str(), tempPaths.tempLogo.c_str(), nullptr,
        aligned.x, aligned.y, aligned.w, aligned.h,
        opt.logoGenThreshold, opt.logoGenSamples,
        ReportAnalyzeProgress) == 0) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: %hs\n"), SafeGetError(api, ctx.get()).c_str());
        g_logoGenProgressState = nullptr;
        return ERR_RUNTIME_NATIVE;
    }
    g_logoGenProgressState = nullptr;

    std::unique_ptr<void, LogoDeleter> logo(api.LogoFile_Create(ctx.get(), tempPaths.tempLogo.c_str()), LogoDeleter{ &api });
    if (!logo) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: %hs\n"), SafeGetError(api, ctx.get()).c_str());
        return ERR_RUNTIME_NATIVE;
    }
    api.LogoFile_SetServiceId(logo.get(), serviceId);
    const std::string name = TruncateNameBytes(MakeLogoNameCp932(api, tsInfo.get(), serviceId));
    api.LogoFile_SetName(logo.get(), name.c_str());
    const int saveOk = opt.aviutlLgd
        ? api.LogoFile_SaveAviUtl(logo.get(), tempPaths.finalTemp.c_str())
        : api.LogoFile_Save(logo.get(), tempPaths.finalTemp.c_str());
    if (saveOk == 0) {
        _ftprintf(stderr, _T("AmatsukazeGenLogo error: %hs\n"), SafeGetError(api, ctx.get()).c_str());
        return ERR_RUNTIME_NATIVE;
    }
    if (!opt.logoImagePath.empty()) {
        constexpr int kJpegQuality = 90;
        constexpr uint8_t kBgBlack = 0;
        if (api.LogoFile_SaveImageJpeg(logo.get(), opt.logoImagePath.c_str(), kJpegQuality, kBgBlack) == 0) {
            PrintCliInfo(_T("warning: ロゴ画像のJPEG保存に失敗しました"));
            const std::string jpegErr = SafeGetError(api, ctx.get());
            if (!jpegErr.empty()) {
                PrintCliInfo(_T("warning: %hs"), jpegErr.c_str());
            }
        } else {
            PrintCliInfo(_T("logo image saved: %s"), opt.logoImagePath.c_str());
        }
    }
    logo.reset();
    {
        fs::path savedPath;
        ret = FinalizeOutput(tempPaths.finalTemp, opt.output, savedPath);
        if (ret != 0) {
            return ret;
        }
        PrintCliInfo(_T("saved: %s"), savedPath.c_str());
    }
    RemoveIfExists(tempPaths.finalTemp);
    RemoveIfExists(tempPaths.tempLogo);
    RemoveIfExists(tempPaths.workFile);
    return 0;
}

} // namespace

int _tmain(int argc, const TCHAR* argv[]) {
#if defined(_WIN32) || defined(_WIN64)
    _tsetlocale(LC_CTYPE, _T(".UTF-8"));
#endif
    Options opt;
    int ret = ParseArgs(argc, argv, opt);
    if (ret != 0) {
        return ret;
    }
    if (opt.showHelp) {
        PrintUsage();
        return 0;
    }
    if (opt.showVersion) {
        PrintVersion();
        return 0;
    }

    fs::path exePath;
    ret = GetExecutablePath(exePath);
    if (ret != 0) {
        return ret;
    }
    const fs::path exeDir = exePath.parent_path();
#if defined(_WIN32) || defined(_WIN64)
    const fs::path dllPath = exeDir / L"Amatsukaze.dll";
#else
    const fs::path dllPath = exeDir / "libAmatsukaze.so";
#endif
    auto module = RGY_LOAD_LIBRARY(dllPath.c_str());
    if (module == nullptr) {
        _ftprintf(stderr, _T("Failed to load %s\n"), dllPath.c_str());
        return 5;
    }

    NativeApi api;
    ret = LoadNativeApi(module, api);
    if (ret != 0) {
        RGY_FREE_LIBRARY(module);
        return ret;
    }
    api.InitAmatsukazeDLL();
    ret = Run(api, opt);
    RGY_FREE_LIBRARY(module);
    return ret;
}
