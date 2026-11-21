/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "NicoJK.h"
#include "OSUtil.h"
#include "rgy_filesystem.h"
#include <filesystem>
#include <iomanip>

namespace {

struct PythonCandidate {
    tstring path;
    bool launcher;
};

static void AddPythonCandidate(std::vector<PythonCandidate>& candidates,
    const tstring& path, bool launcher) {
    if (path.empty() || !rgy_file_exists(path)) {
        return;
    }
    const auto duplicate = std::find_if(candidates.begin(), candidates.end(),
        [&path](const PythonCandidate& candidate) { return candidate.path == path; });
    if (duplicate == candidates.end()) {
        candidates.push_back({ path, launcher });
    }
}

static bool CanRunPython3(const tstring& command) {
    static const std::string marker = "AMATSUKAZE_PYTHON3_OK";
    try {
        const auto args = command
            + _T(" -c \"import sys; print('AMATSUKAZE_PYTHON3_OK') if sys.version_info >= (3, 6) else sys.exit(1)\"");
        StdRedirectedSubProcess process(args, 0, true, false, true);
        if (process.join() != 0) {
            return false;
        }
        const auto& lines = process.getCapturedLines();
        return std::any_of(lines.begin(), lines.end(), [](const std::vector<char>& line) {
            return std::string(line.begin(), line.end()) == marker;
        });
    } catch (Exception&) {
        return false;
    }
}

static bool IsPythonScript(const tstring& path) {
    auto lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](tchar c) { return (c >= 'A' && c <= 'Z') ? (tchar)(c - 'A' + 'a') : c; });
    return ends_with(lower, _T(".py"));
}

static tstring ResolvePythonCommand() {
    std::vector<PythonCandidate> candidates;
    const auto moduleDir = GetModuleDirectory();
#if defined(_WIN32) || defined(_WIN64)
    AddPythonCandidate(candidates, PathCombineS(moduleDir, _T("python.exe")), false);
    AddPythonCandidate(candidates, PathCombineS(moduleDir, _T("python3.exe")), false);
    AddPythonCandidate(candidates, PathCombineS(moduleDir, _T("py.exe")), true);
    AddPythonCandidate(candidates, find_executable_in_path(_T("py.exe")), true);
    AddPythonCandidate(candidates, find_executable_in_path(_T("python.exe")), false);
    AddPythonCandidate(candidates, find_executable_in_path(_T("python3.exe")), false);
#else
    AddPythonCandidate(candidates, PathCombineS(moduleDir, _T("python3")), false);
    AddPythonCandidate(candidates, PathCombineS(moduleDir, _T("python")), false);
    AddPythonCandidate(candidates, find_executable_in_path(_T("python3")), false);
    AddPythonCandidate(candidates, find_executable_in_path(_T("python")), false);
#endif

    for (const auto& candidate : candidates) {
        auto command = _T("\"") + candidate.path + _T("\"");
        if (candidate.launcher) {
            command += _T(" -3");
        }
        if (CanRunPython3(command)) {
            return command;
        }
    }
    return tstring();
}

}

NicoJK::NicoJK(AMTContext& ctx,
    const ConfigWrapper& setting)
    : AMTObject(ctx)
    , setting_(setting)
    , isFail_()
    , jknum_() {}

bool NicoJK::makeASS(int serviceId, time_t startTime, int duration, const StreamReformInfo* reform,
    const std::vector<int>* mainTrims) {
    reform_ = reform;
    mainTrims_ = mainTrims;
    Stopwatch sw;
    sw.start();
    if (!makeASS_(serviceId, startTime, duration)) return false;
    ctx.infoF(_T("コメントASS生成: %.2f秒"), sw.getAndReset());
    readASS();
    ctx.infoF(_T("コメントASS読み込み: %.2f秒"), sw.getAndReset());
    return true;
}

bool NicoJK::isFail() const {
    return isFail_;
}

const std::array<std::vector<std::string>, NICOJK_MAX>& NicoJK::getHeaderLines() const {
    return headerlines_;
}

const std::array<std::vector<NicoJKLine>, NICOJK_MAX>& NicoJK::getDialogues() const {
    return dislogues_;
}
NicoJK::MySubProcess::MySubProcess(const tstring& args)
    : EventBaseSubProcess(args)
    , outConv(false)
    , errConv(true) {}

NicoJK::MySubProcess::~MySubProcess() {
    outConv.Flush();
    errConv.Flush();
}

bool NicoJK::MySubProcess::isFail() const {
    // 対応チャンネルがない場合はエラーにしたくない
    // 何も言わない場合は、対応するチャンネルがないと判断する
    // 他にうまく判別する方法が分からない
    return errConv.nlines > 0;
}
NicoJK::MySubProcess::OutConv::OutConv(bool isErr) : nlines(0), isErr(isErr) {}
/* virtual */ void NicoJK::MySubProcess::OutConv::OnTextLine(const uint8_t* ptr, int len, int brlen, bool /*endedWithCarriageReturnOnly*/) {
    std::vector<char> line = utf8ToString(ptr, len);
    line.push_back('\n');
    fwrite(line.data(), line.size(), 1, SUBPROC_OUT);
    fflush(SUBPROC_OUT);
    nlines++;
}
/* virtual */ void NicoJK::MySubProcess::onOut(bool isErr, MemoryChunk mc) {
    (isErr ? errConv : outConv).AddBytes(mc);
}

void NicoJK::getJKNum(int serviceId) {
    File file(setting_.getNicoConvChSidPath(), _T("r"));
    std::regex re("([^\\t]+)\\t([^\\t]+)\\t([^\\t]+)\\t([^\\t]+)\\t([^\\t]+)");
    std::string str;
    while (file.getline(str)) {
        std::smatch m;
        if (std::regex_search(str, m, re)) {
            int jknum = strtol(m[1].str().c_str(), nullptr, 0);
            int sid = strtol(m[3].str().c_str(), nullptr, 0);
            if (sid == serviceId) {
                jknum_ = jknum;
                // ch_sid.txt の tvname は UTF-8。表示時は CP_UTF8 で変換する
                tvname_ = m[5].str();
                return;
            }
        }
    }
    jknum_ = -1;
}

// 対象の局と取得時刻をログに出す(jk番号がなければ出さない)
void NicoJK::logTargetStation(int serviceId, time_t startTime, int duration) {
    if (jknum_ == -1) return;
    // 取得時刻を表示
    tm t;
#if defined(_WIN32) || defined(_WIN64)
    if (gmtime_s(&t, &startTime) != 0) {
        THROW(RuntimeException, "gmtime_s failed ...");
    }
#else
    if (gmtime_r(&startTime, &t) == NULL) {
        THROW(RuntimeException, "gmtime_r failed ...");
    }
#endif
    t.tm_hour += 9; // GMT+9
    mktime(&t);
    ctx.infoF(_T("%s (jk%d) %d年%02d月%02d日 %02d時%02d分%02d秒 から %d時間%02d分%02d秒"),
        char_to_tstring(tvname_, CP_UTF8), jknum_,
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
        duration / 3600, (duration / 60) % 60, duration % 60);
}

void NicoJK::makeT(NicoJKType srcType, NicoJKType dstType) {
    File file(setting_.getTmpNicoJKASSPath(srcType), _T("r"));
    File dst(setting_.getTmpNicoJKASSPath(dstType), _T("w"));
    std::string str;
    while (file.getline(str)) {
        dst.writeline(str);
        if (str == "[V4+ Styles]") break;
    }

    // Format ...
    file.getline(str);
    dst.writeline(str);

    while (file.getline(str)) {
        if (str.size() >= 6 && str.substr(0, 6) == "Style:") {
            // 変更前
            //|0           |1         |2 |3         |4         |5         |6         |7 |8|9|0|1  |2  |3|4   |5|6|7|8|9 |0 |1 |2|
            // Style: white,MS PGothic,28,&H00ffffff,&H00ffffff,&H00000000,&H00000000,-1,0,0,0,200,200,0,0.00,1,0,4,7,20,20,40,1
            // 変更後
            // Style: white,MS PGothic,28,&H70ffffff,&H70ffffff,&H70000000,&H70000000,-1,0,0,0,200,200,0,0.00,1,1,0,7,20,20,40,1
            auto tokens = split(str, ",");
            for (int i = 3; i < 7; i++) {
                // 透明度（&Hの後の2文字をalpha値に置換）
                auto pos = tokens[i].find("&H");
                if (pos != std::string::npos && pos + 3 < tokens[i].size()) {
                    tokens[i][pos + 2] = '7';
                    tokens[i][pos + 3] = '0';
                }
            }
            tokens[16] = "1"; // Outlineあり
            tokens[17] = "0"; // Shadowなし

            StringBuilder sb;
            for (int i = 0; i < (int)tokens.size(); i++) {
                sb.append("%s%s", i ? "," : "", tokens[i]);
            }
            auto tmp = sb.str();
            dst.writeline(tmp);
        } else {
            dst.writeline(str);
            break;
        }
    }

    // 残り
    while (file.getline(str)) dst.writeline(str);
}

tstring NicoJK::MakeNicoConvASSArgs(ConvMode mode, size_t startTime, NicoJKType type) {
    int width[] = { 1280, 1280, 1920, 1920 };
    int height[] = { 720, 720, 1080, 1080 };
    StringBuilderT sb;
    sb.append(_T("\"%s\" -width %d -height %d -wfilename \"%s\" -chapter 0"),
        setting_.getNicoConvAssPath(),
        width[(int)type], height[(int)type],
        pathToOS(setting_.getTmpNicoJKASSPath(type)));
    if (mode == CONV_ASS_LOG) {
        sb.append(_T(" -nicojk 1"), startTime);
    }
    sb.append(_T(" -tx_starttime %zu"), startTime);
    sb.append(_T(" \"%s\""), pathToOS(setting_.getSrcFilePath()));
    return sb.str();
}

bool NicoJK::nicoConvASS(ConvMode mode, size_t startTime) {
    NicoJKMask mask_i[] = { MASK_720X , MASK_1080X };
    NicoJKType type_s[] = { NICOJK_720S , NICOJK_1080S };
    NicoJKMask mask_t[] = { MASK_720T , MASK_1080T };
    NicoJKType type_t[] = { NICOJK_720T , NICOJK_1080T };

    int typemask = setting_.getNicoJKMask();
    for (int i = 0; i < 2; i++) {
        if (mask_i[i] & typemask) {
            auto args = MakeNicoConvASSArgs(mode, startTime, type_s[i]);
            ctx.infoF(_T("%s"), args);
            MySubProcess process(args);
            int exitCode = process.join();
            if (exitCode == 0 && File::exists(setting_.getTmpNicoJKASSPath(type_s[i]))) {
                if (mask_t[i] & typemask) {
                    makeT(type_s[i], type_t[i]);
                }
                continue;
            }
            isFail_ = process.isFail();
            return false;
        }
    }

    return true;
}

/* static */ double NicoJK::toClock(int h, int m, int s, int ss) {
    return (((((h * 60.0) + m) * 60.0) + s) * 100.0 + ss) * 900.0;
}

void NicoJK::readASS() {
    int typemask = setting_.getNicoJKMask();
    for (int i = 0; i < NICOJK_MAX; i++) {
        if ((1 << i) & typemask) {
            File file(setting_.getTmpNicoJKASSPath((NicoJKType)i), _T("r"));
            std::string str;
            while (file.getline(str)) {
                headerlines_[i].push_back(str);
                if (str == "[Events]") break;
            }

            // Format ...
            file.getline(str);
            headerlines_[i].push_back(str);

            std::regex re("Dialogue: 0,(\\d+):(\\d\\d):(\\d\\d)\\.(\\d\\d),(\\d+):(\\d\\d):(\\d\\d)\\.(\\d\\d)(.*)");

            while (file.getline(str)) {
                std::smatch m;
                if (std::regex_search(str, m, re)) {
                    NicoJKLine elem = {
                        toClock(
                            std::stoi(m[1].str()), std::stoi(m[2].str()),
                            std::stoi(m[3].str()), std::stoi(m[4].str())),
                        toClock(
                            std::stoi(m[5].str()), std::stoi(m[6].str()),
                            std::stoi(m[7].str()), std::stoi(m[8].str())),
                        m[9].str()
                    };
                    dislogues_[i].push_back(elem);
                }
            }
        }
    }
}

// 元TSを基準に補助ファイルを読み、CLIの項目を最後に重ねる。
bool NicoJK::resolveSource() {
    const auto base = PathRemoveExtensionS(setting_.getSrcFileOriginalPath());
    const auto ini = base + _T(".nicojk.ini");
    NicoJKSourceSpec defaults;
    if (File::exists(ini)) {
        File input(ini, _T("r"));
        std::vector<std::string> lines;
        std::string line;
        while (input.getline(line)) lines.push_back(line);
        defaults = ParseNicoJKIni(lines);
        if (!defaults.file.empty()) {
            auto path = std::filesystem::path(defaults.file);
            if (path.is_relative()) path = std::filesystem::path(ini).parent_path() / path;
            defaults.file = path.lexically_normal().native();
        }
    }
    if (!defaults.ref.empty()) {
        auto path = std::filesystem::path(defaults.ref);
        if (path.is_relative()) path = std::filesystem::path(ini).parent_path() / path;
        defaults.ref = path.lexically_normal().native();
    }
    source_ = MergeNicoJKSource(setting_.getNicoJKSource(), defaults);
    const bool explicitFile = !source_.file.empty();
    if (!explicitFile && source_.ref.empty()) {
        const tchar* suffixes[] = { _T(".nicojk.xml"), _T(".nicojk.jkl"), _T(".xml"), _T(".jkl") };
        for (int i = 0; i < 4; ++i) {
            const auto candidate = base + suffixes[i];
            if (!File::exists(candidate)) continue;
            source_.file = candidate;
            source_.strictFile = i < 2;
            break;
        }
    }
    // 別用途の同名XMLだけならPython未導入でも従来経路へ戻せるようにする。
    if (!source_.file.empty() && !source_.strictFile) {
        File input(source_.file, _T("r"));
        std::string line;
        bool hasChat = false;
        const std::regex chatTag("<chat([[:space:]]|>)");
        while (input.getline(line)) {
            if (std::regex_search(line, chatTag)) { hasChat = true; break; }
        }
        if (!hasChat) {
            ctx.warn(_T("同名ファイルはコメントファイルではないため無視します"));
            source_.file.clear();
        }
    }
    if (!source_.specified && source_.file.empty()) return false;
    // 取得範囲と時刻の合わせ方(コメントファイルの検査を含む)はnicojk_assのmakeが決める。
    if (!source_.ref.empty() && source_.file.empty() && !canRunScript()) {
        ctx.warn(_T("nicojk_assまたはPython 3が見つからないため、基準録画を使わず通常の実況取得に戻ります"));
        return false;
    }
    const auto script = getNicoJKAssScriptPath();
    if (!File::exists(script)) THROWF(RuntimeException, "nicojk_assが見つかりません: %s", script);
    return true;
}

bool NicoJK::canRunScript() {
    const auto script = getNicoJKAssScriptPath();
    if (!File::exists(script)) return false;
    // exeは直接起動するので、Pythonが要るのは.pyを指定したときだけ
    return !IsPythonScript(script) || !ResolvePythonCommand().empty();
}

// 入力ファイルのフォルダと、その移動先のsucceeded・failed(あれば)
std::vector<tstring> NicoJK::getSearchDirs() const {
    std::vector<tstring> dirs;
    const auto dir = std::filesystem::path(setting_.getSrcFileOriginalPath()).parent_path();
    dirs.push_back(dir.empty() ? tstring(_T(".")) : dir.native());
    for (const auto name : { _T("succeeded"), _T("failed") }) {
        std::error_code ec;
        const auto sub = dir / name;
        if (std::filesystem::is_directory(sub, ec)) dirs.push_back(sub.native());
    }
    return dirs;
}

tstring NicoJK::getNicoJKAssScriptPath() const {
    auto script = setting_.getNicoJKAssPath();
#if defined(_WIN32) || defined(_WIN64)
    const tstring exeName = _T("nicojk_ass.exe");
#else
    const tstring exeName = _T("nicojk_ass");
#endif
    if (script.empty()) script = PathCombineS(GetModuleDirectory(), exeName);
    return script;
}

bool NicoJK::makeASS_(int serviceId, time_t startTime, int duration) {
    try {
        const auto dumpDir = getDumpDir();
        if (!dumpDir.empty()) {
            std::filesystem::create_directories(dumpDir);
        }
        const bool manual = resolveSource();
        // 取得元の指定がないアニメなどは、nicojk_assが先行放送の録画を探して基準にする。
        altMode_.clear();
        if (!manual && setting_.isNicoJKAssEnabled() && !source_.alt.empty()) {
            if (canRunScript()) {
                altMode_ = source_.alt;
            } else {
                ctx.warn(_T("nicojk_assまたはPython 3が見つからないため、別の局のコメントを使わず通常の実況取得に戻ります"));
            }
        }
        if (manual || !altMode_.empty()) {
            if (manual && setting_.isUseNicoJKLog()) ctx.info(_T("取得元の指定をnicojklogより優先します"));
            // 基準の録画を使うときだけ、対象の音声を渡す
            const bool useReference = !source_.ref.empty() || !altMode_.empty();
            // デバッグ用: 先行放送の録画があっても、対象の音声を渡さずコメントでの照合だけにする
            const char* commentAlignDebug = std::getenv("AMATSUKAZE_NICOJK_COMMENT_ALIGN_DEBUG");
            const bool commentAlignOnly = !altMode_.empty() && commentAlignDebug && std::string(commentAlignDebug) == "1";
            if (commentAlignOnly) {
                ctx.info(_T("AMATSUKAZE_NICOJK_COMMENT_ALIGN_DEBUG=1のため、音声を使わずコメントで時刻を合わせます"));
                targetWavPath_.clear();
            }
            if (!useReference || commentAlignOnly || prepareTargetAudio()) {
                // 先行放送のコメントでは、対象の局にjk番号がなくても基準があればコメントを付けられる
                // (局の表が読めなくても、nicojk_assが自分の表で判断するので続ける)
                if (!altMode_.empty()) {
                    try {
                        getJKNum(serviceId);
                    } catch (Exception&) {
                        jknum_ = -1;
                    }
                    logTargetStation(serviceId, startTime, duration);
                }
                prepareSpans();
                return makeASSByScript(serviceId, startTime, duration);
            }
        }
        // 基準の準備や実行環境が使えない場合も、取得元を残さず元のAPI/旧経路へ戻る。
        source_ = NicoJKSourceSpec();
        altMode_.clear();
        targetWavPath_.clear();
        spansPath_.clear();
    } catch (Exception& e) {
        isFail_ = true;
        ctx.errorF(_T("実況コメントの生成に失敗: %s"), e.message());
        return false;
    }
    // NicoConvASSも指定されていれば、通常の取得はNicoConvASSで行う(nicojk_assは先行放送のコメントと手動の取得元だけ)
    if (setting_.isNicoJKAssEnabled() && !setting_.isNicoConvAssPreferred()) {
        getJKNum(serviceId);
        // 先行放送のコメントを使わないなら、対象の局にjk番号がないときはnicojk_assを呼ばず終わる
        if (jknum_ == -1) return false;
        logTargetStation(serviceId, startTime, duration);
        return makeASSByScript(serviceId, startTime, duration);
    }

    return nicoConvASS(setting_.isUseNicoJKLog() ? CONV_ASS_LOG : CONV_ASS_TS, startTime);
}

// nicojk_ass のmakeに渡すコマンドライン引数を生成する(全解像度の出力を1回で指定する)
tstring NicoJK::MakeNicoJKScriptArgs(const tstring& pythonCommand,
    int serviceId, time_t startTime, int duration) {
    // .pyならPythonで動かし、exeならそのまま起動する
    auto args = pythonCommand.empty()
        ? StringFormat(_T("\"%s\" make"), pathToOS(getNicoJKAssScriptPath()))
        : StringFormat(_T("%s \"%s\" make"), pythonCommand.c_str(), pathToOS(getNicoJKAssScriptPath()));
    // 対象
    if (source_.channel) args += StringFormat(_T(" --channel jk%d"), *source_.channel);
    else args += StringFormat(_T(" --sid %d"), serviceId);
    args += StringFormat(_T(" --start %lld --end %lld"), (long long)startTime, (long long)startTime + duration);
    const bool hasRef = !source_.ref.empty();
    if (!hasRef) {
        if (source_.programStart) args += StringFormat(_T(" --program-start %.6f"), *source_.programStart);
        if (!source_.file.empty()) {
            args += StringFormat(_T(" --comments-file \"%s\""), pathToOS(source_.file));
            // 自動で見つけた同名ファイルは、コメントがなければnicojk_ass側で使わない
            if (!source_.strictFile) args += _T(" --comments-file-auto");
        }
        if (source_.start) args += StringFormat(_T(" --comments-start %.6f"), *source_.start);
        if (source_.offset) args += StringFormat(_T(" --comments-offset %.6f"), *source_.offset);
    }
    // 基準
    if (!altMode_.empty()) {
        // 録画の探索はnicojk_assが行う。入力ファイル自身は除く。
        args += StringFormat(_T(" --alt %s"), altMode_.c_str());
        for (const auto& dir : getSearchDirs()) {
            args += StringFormat(_T(" --search-dir \"%s\""), pathToOS(dir));
        }
        args += StringFormat(_T(" --exclude \"%s\""), pathToOS(setting_.getSrcFileOriginalPath()));
    } else if (hasRef) {
        args += StringFormat(_T(" --ref-ts \"%s\""), pathToOS(source_.ref));
        if (source_.refSid) args += StringFormat(_T(" --ref-sid %d"), *source_.refSid);
        if (!source_.file.empty()) args += StringFormat(_T(" --ref-comments-file \"%s\""), pathToOS(source_.file));
        args += StringFormat(_T(" --mode %s"), source_.refMode == _T("replace") ? _T("replace") : _T("merge"));
    }
    if (!targetWavPath_.empty() && (hasRef || !altMode_.empty())) {
        // 基準の録画のデコードは、nicojk_assがこのCLI自身を--mode nicojk_wavで呼んで行う
        args += StringFormat(_T(" --wav \"%s\" --decoder \"amatsukaze=%s\""),
            pathToOS(targetWavPath_), pathToOS(getModulePath(nullptr)));
    }
    if (!spansPath_.empty()) args += StringFormat(_T(" --spans \"%s\""), pathToOS(spansPath_));
    const int widths[] = { 1280, 1920 }, heights[] = { 720, 1080 };
    const NicoJKMask masks[] = { MASK_720X, MASK_1080X };
    const NicoJKType types[] = { NICOJK_720S, NICOJK_1080S };
    for (int i = 0; i < 2; i++) {
        if (masks[i] & setting_.getNicoJKMask()) {
            args += StringFormat(_T(" --output \"%dx%d=%s\""), widths[i], heights[i], pathToOS(setting_.getTmpNicoJKASSPath(types[i])));
        }
    }
    const auto dumpDir = getDumpDir();
    if (!dumpDir.empty()) {
        args += StringFormat(_T(" --work-dir \"%s\" --keep-work --report \"%s\""),
            pathToOS(dumpDir), pathToOS(getWorkFilePath(_T("report.json"))));
    }
    const char* markDebug = std::getenv("AMATSUKAZE_NICOJK_MERGE_DEBUG");
    if (markDebug && std::string(markDebug) == "1") args += _T(" --mark-reference-color yellow");
    return args;
}

// nicojk_ass の make を1回呼び出して ASS ファイルを生成する
bool NicoJK::makeASSByScript(int serviceId, time_t startTime, int duration) {
    const NicoJKMask mask_i[] = { MASK_720X,  MASK_1080X  };
    const NicoJKType type_s[] = { NICOJK_720S, NICOJK_1080S };
    const NicoJKMask mask_t[] = { MASK_720T,  MASK_1080T  };
    const NicoJKType type_t[] = { NICOJK_720T, NICOJK_1080T };

    // exeは直接起動するのでPythonを探さない。.pyを指定したときだけPythonで動かす(開発用)
    tstring pythonCommand;
    if (IsPythonScript(getNicoJKAssScriptPath())) {
        pythonCommand = ResolvePythonCommand();
        if (pythonCommand.empty()) {
            THROW(RuntimeException,
                "nicojk_assの実行に必要なPython 3が見つかりません。"
                "exe_filesにPythonを配置するか、Pythonの実行ファイルにPATHを通してください。");
        }
        // 出力の文字コードをUTF-8にそろえる(exeは常にUTF-8で出力する)
#if defined(_WIN32) || defined(_WIN64)
        _putenv_s("PYTHONIOENCODING", "utf-8");
#else
        setenv("PYTHONIOENCODING", "utf-8", 1);
#endif
    }

    const auto args = MakeNicoJKScriptArgs(pythonCommand, serviceId, startTime, duration);
    ctx.infoF(_T("%s"), args);
    // nicojk_assの出力はUTF-8なので、ログの文字コードへ変換して流す
    StdRedirectedSubProcess process(args, 0, true);
    const int exitCode = process.join();
    if (exitCode == 100) {
        // 付けるコメントがない (nicojk_ass側で 100 を返す)
        return false;
    }
    if (exitCode != 0) {
        isFail_ = true;
        return false;
    }
    const int typemask = setting_.getNicoJKMask();
    for (int i = 0; i < 2; i++) {
        if (!(mask_i[i] & typemask)) continue;
        if (!File::exists(setting_.getTmpNicoJKASSPath(type_s[i]))) {
            isFail_ = true;
            return false;
        }
        // ASS 生成成功。必要なら透過スタイル版 (T) も生成する
        if (mask_t[i] & typemask) {
            makeT(type_s[i], type_t[i]);
        }
    }
    return true;
}
NicoJKFormatter::NicoJKFormatter(AMTContext& ctx)
    : AMTObject(ctx) {}

std::string NicoJKFormatter::generate(
    const std::vector<std::string>& headers,
    const std::vector<NicoJKLine>& dialogues) {
    sb.clear();
    for (auto& header : headers) {
        sb.append("%s\n", header);
    }
    for (auto& dialogue : dialogues) {
        sb.append("Dialogue: 0,");
        time(dialogue.start);
        sb.append(",");
        time(dialogue.end);
        sb.append("%s\n", dialogue.line);
    }
    return sb.str();
}

void NicoJKFormatter::time(double t) {
    double totalSec = t / MPEG_CLOCK_HZ;
    double totalMin = totalSec / 60;
    int h = (int)(totalMin / 60);
    int m = (int)totalMin % 60;
    double sec = totalSec - (int)totalMin * 60;
    sb.append("%d:%02d:%05.2f", h, m, sec);
}
