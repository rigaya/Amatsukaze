/**
* Amtasukaze GenLogo CLI Entry point
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "rgy_osdep.h"
#include "rgy_tchar.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
    AutoDetectLogoRectFunc AutoDetectLogoRect = nullptr;
    LogoFileCreateFunc LogoFile_Create = nullptr;
    LogoFileDeleteFunc LogoFile_Delete = nullptr;
    LogoFileSetServiceIdFunc LogoFile_SetServiceId = nullptr;
    LogoFileSetNameFunc LogoFile_SetName = nullptr;
    LogoFileSaveFunc LogoFile_Save = nullptr;
    LogoFileSaveFunc LogoFile_SaveAviUtl = nullptr;
};

bool AlwaysContinueAnalyze(float, int, int, int) {
    return true;
}

bool AlwaysContinueAutoDetect(int, float, float, int, int) {
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
        _T("      --auto-logo-detect-threads <n>        自動ロゴ枠検出スレッド数 [0=min(論理コア数,16)]\n")
        _T("\n")
        _T("Logo generate options:\n")
        _T("      --logo-gen-threshold <n>              ロゴ生成の閾値 [auto-detect-thresholdと同値]\n")
        _T("      --logo-gen-samples <n>                ロゴ生成の最大サンプル数 [search-framesと同値]\n")
        _T("\n")
        _T("Other options:\n")
        _T("      --aviutl-lgd                          AviUtl向けlgdを保存\n")
        _T("      --debug-dir <path>                    自動ロゴ枠検出デバッグ画像出力先\n")
        _T("      --help                                このヘルプを表示\n")
        _T("      --version                             バージョンを表示\n"));
}

void PrintVersion() {
#if defined(_WIN32) || defined(_WIN64)
    _ftprintf(stdout, _T("AmatsukazeGenLogo %S\n"), kVersion);
#else
    _ftprintf(stdout, _T("AmatsukazeGenLogo %s\n"), kVersion);
#endif
}

std::string ToErrorString(const tstring& message) {
#if defined(_WIN32) || defined(_WIN64)
    if (message.empty()) {
        return std::string();
    }
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) {
        return std::string("argument error");
    }
    std::string utf8((size_t)utf8Len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, message.c_str(), -1, utf8.data(), utf8Len, nullptr, nullptr);
    return utf8;
#else
    return message;
#endif
}

[[noreturn]] void ThrowArgError(const tstring& message) {
    throw std::runtime_error(ToErrorString(message));
}

int ParseInt(const tstring& value, const TCHAR* optionName) {
    try {
        size_t pos = 0;
        const int result = std::stoi(value, &pos, 10);
        if (pos != value.size()) {
            ThrowArgError(tstring(optionName) + _T(" の値が不正です"));
        }
        return result;
    } catch (...) {
        ThrowArgError(tstring(optionName) + _T(" の値が不正です"));
    }
}

Rect ParseRect(const tstring& value) {
    std::array<int, 4> values = {};
    size_t start = 0;
    for (int i = 0; i < 4; i++) {
        size_t end = value.find(_T(','), start);
        if (i == 3) {
            end = tstring::npos;
        } else if (end == tstring::npos) {
            ThrowArgError(_T("--logo-range の形式は x,y,w,h です"));
        }
        const tstring part = value.substr(start, (end == tstring::npos) ? tstring::npos : (end - start));
        values[i] = ParseInt(part, _T("--logo-range"));
        start = (end == tstring::npos) ? end : end + 1;
    }
    if (values[2] <= 0 || values[3] <= 0) {
        ThrowArgError(_T("--logo-range の幅と高さは1以上で指定してください"));
    }
    return Rect{ values[0], values[1], values[2], values[3] };
}

Options ParseArgs(int argc, const TCHAR* argv[]) {
    Options opt;
    for (int i = 1; i < argc; i++) {
        const tstring key = argv[i];
        auto requireValue = [&](const TCHAR* optionName) -> tstring {
            if (i + 1 >= argc) {
                ThrowArgError(tstring(optionName) + _T(" の値が不足しています"));
            }
            return argv[++i];
        };

        if (key == _T("-i") || key == _T("--input")) {
            opt.input = requireValue(key.c_str());
        } else if (key == _T("-o") || key == _T("--output")) {
            opt.output = requireValue(key.c_str());
        } else if (key == _T("--logo-range")) {
            opt.logoRange = ParseRect(requireValue(key.c_str()));
        } else if (key == _T("--auto-logo-detect-search-frames")) {
            opt.autoSearchFrames = ParseInt(requireValue(key.c_str()), key.c_str());
        } else if (key == _T("--auto-logo-detect-div-x")) {
            opt.autoDivX = ParseInt(requireValue(key.c_str()), key.c_str());
        } else if (key == _T("--auto-logo-detect-div-y")) {
            opt.autoDivY = ParseInt(requireValue(key.c_str()), key.c_str());
        } else if (key == _T("--auto-logo-detect-block-size")) {
            opt.autoBlockSize = ParseInt(requireValue(key.c_str()), key.c_str());
        } else if (key == _T("--auto-logo-detect-threshold")) {
            opt.autoThreshold = ParseInt(requireValue(key.c_str()), key.c_str());
        } else if (key == _T("--auto-logo-detect-margin-x")) {
            opt.autoMarginX = ParseInt(requireValue(key.c_str()), key.c_str());
        } else if (key == _T("--auto-logo-detect-margin-y")) {
            opt.autoMarginY = ParseInt(requireValue(key.c_str()), key.c_str());
        } else if (key == _T("--auto-logo-detect-threads")) {
            opt.autoThreads = ParseInt(requireValue(key.c_str()), key.c_str());
        } else if (key == _T("--logo-gen-threshold")) {
            opt.logoGenThreshold = ParseInt(requireValue(key.c_str()), key.c_str());
        } else if (key == _T("--logo-gen-samples")) {
            opt.logoGenSamples = ParseInt(requireValue(key.c_str()), key.c_str());
        } else if (key == _T("--aviutl-lgd")) {
            opt.aviutlLgd = true;
        } else if (key == _T("--debug-dir")) {
            opt.debugDir = requireValue(key.c_str());
        } else if (key == _T("--help")) {
            opt.showHelp = true;
        } else if (key == _T("--version")) {
            opt.showVersion = true;
        } else {
            ThrowArgError(_T("不明なオプションです: ") + key);
        }
    }

    if (opt.showHelp || opt.showVersion) {
        return opt;
    }
    if (opt.input.empty()) {
        ThrowArgError(_T("--input を指定してください"));
    }
    if (opt.output.empty()) {
        ThrowArgError(_T("--output を指定してください"));
    }
    if (opt.autoDivX <= 0 || opt.autoDivY <= 0) {
        ThrowArgError(_T("div-x/div-y は1以上で指定してください"));
    }
    if (opt.autoSearchFrames < 100) {
        ThrowArgError(_T("search-frames は100以上で指定してください"));
    }
    if (opt.autoBlockSize < 4) {
        ThrowArgError(_T("block-size は4以上で指定してください"));
    }
    if (opt.autoThreshold < 1) {
        ThrowArgError(_T("auto-detect-threshold は1以上で指定してください"));
    }
    if (opt.autoMarginX < 0 || opt.autoMarginY < 0) {
        ThrowArgError(_T("margin-x/margin-y は0以上で指定してください"));
    }
    if (opt.autoThreads < 0) {
        ThrowArgError(_T("auto-logo-detect-threads は0以上で指定してください"));
    }
    if (opt.logoGenThreshold == -1) {
        opt.logoGenThreshold = opt.autoThreshold;
    }
    if (opt.logoGenSamples == -1) {
        opt.logoGenSamples = opt.autoSearchFrames;
    }
    if (opt.logoGenThreshold < 1) {
        ThrowArgError(_T("logo-gen-threshold は1以上で指定してください"));
    }
    if (opt.logoGenSamples < 1) {
        ThrowArgError(_T("logo-gen-samples は1以上で指定してください"));
    }
    return opt;
}

fs::path GetExecutablePath() {
#if defined(_WIN32) || defined(_WIN64)
    std::wstring buffer(32768, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    buffer.resize(len);
    return fs::path(buffer);
#else
    std::vector<char> buffer(32768, '\0');
    ssize_t len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len <= 0) {
        throw std::runtime_error("readlink(/proc/self/exe) failed");
    }
    buffer[(size_t)len] = '\0';
    return fs::path(buffer.data());
#endif
}

template<typename T>
T LoadSymbol(HMODULE module, const char* name) {
    auto symbol = reinterpret_cast<T>(RGY_GET_PROC_ADDRESS(module, name));
    if (symbol == nullptr) {
        throw std::runtime_error(std::string("Failed to load symbol: ") + name);
    }
    return symbol;
}

NativeApi LoadNativeApi(HMODULE module) {
    NativeApi api;
    api.InitAmatsukazeDLL = LoadSymbol<InitAmatsukazeDLLFunc>(module, "InitAmatsukazeDLL");
    api.AMTContext_Create = LoadSymbol<AMTContextCreateFunc>(module, "AMTContext_Create");
    api.ATMContext_Delete = LoadSymbol<AMTContextDeleteFunc>(module, "ATMContext_Delete");
    api.AMTContext_GetError = LoadSymbol<AMTContextGetErrorFunc>(module, "AMTContext_GetError");
    api.TsInfo_Create = LoadSymbol<TsInfoCreateFunc>(module, "TsInfo_Create");
    api.TsInfo_Delete = LoadSymbol<TsInfoDeleteFunc>(module, "TsInfo_Delete");
    api.TsInfo_ReadFile = LoadSymbol<TsInfoReadFileFunc>(module, "TsInfo_ReadFile");
    api.TsInfo_HasServiceInfo = LoadSymbol<TsInfoHasServiceInfoFunc>(module, "TsInfo_HasServiceInfo");
    api.TsInfo_GetDay = LoadSymbol<TsInfoGetDayFunc>(module, "TsInfo_GetDay");
    api.TsInfo_GetTime = LoadSymbol<TsInfoGetTimeFunc>(module, "TsInfo_GetTime");
    api.TsInfo_GetNumProgram = LoadSymbol<TsInfoGetNumProgramFunc>(module, "TsInfo_GetNumProgram");
    api.TsInfo_GetProgramInfo = LoadSymbol<TsInfoGetProgramInfoFunc>(module, "TsInfo_GetProgramInfo");
    api.TsInfo_GetNumService = LoadSymbol<TsInfoGetNumServiceFunc>(module, "TsInfo_GetNumService");
    api.TsInfo_GetServiceId = LoadSymbol<TsInfoGetServiceIdFunc>(module, "TsInfo_GetServiceId");
    api.TsInfo_GetServiceName = LoadSymbol<TsInfoGetServiceNameFunc>(module, "TsInfo_GetServiceName");
    api.ScanLogo = LoadSymbol<ScanLogoFunc>(module, "ScanLogo");
    api.AutoDetectLogoRect = LoadSymbol<AutoDetectLogoRectFunc>(module, "AutoDetectLogoRect");
    api.LogoFile_Create = LoadSymbol<LogoFileCreateFunc>(module, "LogoFile_Create");
    api.LogoFile_Delete = LoadSymbol<LogoFileDeleteFunc>(module, "LogoFile_Delete");
    api.LogoFile_SetServiceId = LoadSymbol<LogoFileSetServiceIdFunc>(module, "LogoFile_SetServiceId");
    api.LogoFile_SetName = LoadSymbol<LogoFileSetNameFunc>(module, "LogoFile_SetName");
    api.LogoFile_Save = LoadSymbol<LogoFileSaveFunc>(module, "LogoFile_Save");
    api.LogoFile_SaveAviUtl = LoadSymbol<LogoFileSaveFunc>(module, "LogoFile_SaveAviUtl");
    return api;
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

int ResolveServiceId(const NativeApi& api, void* tsInfo) {
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
        return videoPrograms[0];
    }

    const int numServices = api.TsInfo_GetNumService(tsInfo);
    if (numServices == 1) {
        return api.TsInfo_GetServiceId(tsInfo, 0);
    }

    throw std::runtime_error("service id を一意に解決できません");
}

void RemoveIfExists(const tstring& path) {
    if (!path.empty()) {
        std::error_code ec;
        fs::remove(fs::path(path), ec);
    }
}

void FinalizeOutput(const tstring& tempPath, const tstring& outputPath) {
    std::error_code ec;
    fs::remove(fs::path(outputPath), ec);
    ec.clear();
    fs::rename(fs::path(tempPath), fs::path(outputPath), ec);
    if (!ec) {
        return;
    }
    fs::copy_file(fs::path(tempPath), fs::path(outputPath), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error("出力ファイルの配置に失敗しました");
    }
    fs::remove(fs::path(tempPath), ec);
}

int Run(const NativeApi& api, const Options& opt) {
    const fs::path inputPath(opt.input);
    if (!fs::exists(inputPath)) {
        throw std::runtime_error("入力ファイルが存在しません");
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

    void* ctx = api.AMTContext_Create();
    if (ctx == nullptr) {
        throw std::runtime_error("AMTContext_Create に失敗しました");
    }

    try {
        void* tsInfo = api.TsInfo_Create(ctx);
        if (tsInfo == nullptr) {
            throw std::runtime_error(SafeGetError(api, ctx));
        }
        try {
            if (api.TsInfo_ReadFile(tsInfo, opt.input.c_str()) == 0) {
                throw std::runtime_error(SafeGetError(api, ctx));
            }

            const int serviceId = ResolveServiceId(api, tsInfo);
            Rect rect = opt.logoRange.value_or(Rect{});
            if (!opt.logoRange.has_value()) {
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

                if (api.AutoDetectLogoRect(
                    ctx, opt.input.c_str(), serviceId,
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
                    AlwaysContinueAutoDetect) == 0) {
                    throw std::runtime_error(SafeGetError(api, ctx));
                }
                rect = Rect{ x, y, w, h };
            }

            const Rect aligned = AlignRect(rect);
            if (aligned.w <= 0 || aligned.h <= 0) {
                throw std::runtime_error("ロゴ枠が不正です");
            }

            if (api.ScanLogo(
                ctx, opt.input.c_str(), serviceId,
                tempPaths.workFile.c_str(), tempPaths.tempLogo.c_str(), nullptr,
                aligned.x, aligned.y, aligned.w, aligned.h,
                opt.logoGenThreshold, opt.logoGenSamples,
                AlwaysContinueAnalyze) == 0) {
                throw std::runtime_error(SafeGetError(api, ctx));
            }

            void* logo = api.LogoFile_Create(ctx, tempPaths.tempLogo.c_str());
            if (logo == nullptr) {
                throw std::runtime_error(SafeGetError(api, ctx));
            }
            try {
                api.LogoFile_SetServiceId(logo, serviceId);
                const std::string name = TruncateNameBytes(MakeLogoNameCp932(api, tsInfo, serviceId));
                api.LogoFile_SetName(logo, name.c_str());
                const int saveOk = opt.aviutlLgd
                    ? api.LogoFile_SaveAviUtl(logo, tempPaths.finalTemp.c_str())
                    : api.LogoFile_Save(logo, tempPaths.finalTemp.c_str());
                if (saveOk == 0) {
                    throw std::runtime_error(SafeGetError(api, ctx));
                }
            } catch (...) {
                api.LogoFile_Delete(logo);
                throw;
            }
            api.LogoFile_Delete(logo);
            FinalizeOutput(tempPaths.finalTemp, opt.output);
        } catch (...) {
            api.TsInfo_Delete(tsInfo);
            throw;
        }
        api.TsInfo_Delete(tsInfo);
    } catch (...) {
        api.ATMContext_Delete(ctx);
        RemoveIfExists(tempPaths.finalTemp);
        RemoveIfExists(tempPaths.tempLogo);
        RemoveIfExists(tempPaths.workFile);
        throw;
    }

    api.ATMContext_Delete(ctx);
    RemoveIfExists(tempPaths.tempLogo);
    RemoveIfExists(tempPaths.workFile);
    return 0;
}

} // namespace

int _tmain(int argc, const TCHAR* argv[]) {
    try {
        const Options opt = ParseArgs(argc, argv);
        if (opt.showHelp) {
            PrintUsage();
            return 0;
        }
        if (opt.showVersion) {
            PrintVersion();
            return 0;
        }

        const fs::path exeDir = GetExecutablePath().parent_path();
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

        try {
            const NativeApi api = LoadNativeApi(module);
            api.InitAmatsukazeDLL();
            const int result = Run(api, opt);
            RGY_FREE_LIBRARY(module);
            return result;
        } catch (const std::exception& ex) {
            fprintf(stderr, "AmatsukazeGenLogo error: %s\n", ex.what());
            RGY_FREE_LIBRARY(module);
            return 1;
        }
    } catch (const std::exception& ex) {
        fprintf(stderr, "AmatsukazeGenLogo error: %s\n", ex.what());
        return 1;
    }
}
