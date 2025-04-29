/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "NicoJK.h"

NicoJK::NicoJK(AMTContext& ctx,
    const ConfigWrapper& setting)
    : AMTObject(ctx)
    , setting_(setting)
    , isFail_()
    , jknum_() {}

bool NicoJK::makeASS(int serviceId, time_t startTime, int duration) {
    Stopwatch sw;
    sw.start();
    if (!makeASS_(sw, serviceId, startTime, duration)) return false;
    ctx.infoF("コメントASS生成: %.2f秒", sw.getAndReset());
    readASS();
    ctx.infoF("コメントASS読み込み: %.2f秒", sw.getAndReset());
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
/* virtual */ void NicoJK::MySubProcess::OutConv::OnTextLine(const uint8_t* ptr, int len, int brlen) {
    std::vector<char> line = utf8ToString(ptr, len);
    line.push_back('\n');
    fwrite(line.data(), line.size(), 1, SUBPROC_OUT);
    fflush(SUBPROC_OUT);
    ++nlines;
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
                tvname_ = m[5].str();
                return;
            }
        }
    }
    jknum_ = -1;
}

tstring NicoJK::MakeNicoJK18Args(int jknum, size_t startTime, size_t endTime) {
    return StringFormat(_T("\"%s\" jk%d %zu %zu -x -f \"%s\""),
        pathNormalize(GetModuleDirectory()) + _T("/NicoJK18Client.exe"),
        jknum, startTime, endTime,
        pathToOS(setting_.getTmpNicoJKXMLPath()));
}

bool NicoJK::getNicoJKXml(time_t startTime, int duration) {
    auto args = MakeNicoJK18Args(jknum_, (size_t)startTime, (size_t)startTime + duration);
    ctx.infoF("%s", args);
    StdRedirectedSubProcess process(args);
    int exitCode = process.join();
    if (exitCode == 0 && File::exists(setting_.getTmpNicoJKXMLPath())) {
        return true;
    }
    if (exitCode == 100) {
        // チャンネルがない
        return false;
    }
    isFail_ = true;
    return false;
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
            for (int i = 3; i < 7; ++i) {
                // 透明度
                tokens[i][2] = '7';
                tokens[i][3] = '0';
            }
            tokens[16] = "1"; // Outlineあり
            tokens[17] = "0"; // Shadowなし

            StringBuilder sb;
            for (int i = 0; i < (int)tokens.size(); ++i) {
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
    sb.append(_T(" \"%s\""), pathToOS(
        (mode != CONV_ASS_XML) ? setting_.getSrcFilePath() : setting_.getTmpNicoJKXMLPath()));
    return sb.str();
}

bool NicoJK::nicoConvASS(ConvMode mode, size_t startTime) {
    NicoJKMask mask_i[] = { MASK_720X , MASK_1080X };
    NicoJKType type_s[] = { NICOJK_720S , NICOJK_1080S };
    NicoJKMask mask_t[] = { MASK_720T , MASK_1080T };
    NicoJKType type_t[] = { NICOJK_720T , NICOJK_1080T };

    int typemask = setting_.getNicoJKMask();
    for (int i = 0; i < 2; ++i) {
        if (mask_i[i] & typemask) {
            auto args = MakeNicoConvASSArgs(mode, startTime, type_s[i]);
            ctx.infoF("%s", args);
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
    for (int i = 0; i < NICOJK_MAX; ++i) {
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

            std::regex re("Dialogue: 0,(\\d):(\\d\\d):(\\d\\d)\\.(\\d\\d),(\\d):(\\d\\d):(\\d\\d)\\.(\\d\\d)(.*)");

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

bool NicoJK::makeASS_(Stopwatch& sw, int serviceId, time_t startTime, int duration) {
    if (setting_.isUseNicoJKLog()) {
        if (nicoConvASS(CONV_ASS_LOG, startTime)) return true;
    }
    if (setting_.isNicoJK18Enabled()) {
        getJKNum(serviceId);
        if (jknum_ == -1) return false;

        // 取得時刻を表示
        tm t;
#if defined(_WIN32) || defined(_WIN64)
        if (gmtime_s(&t, &startTime) != 0) {
#else
        if (gmtime_r(&startTime, &t) == NULL) {
#endif
            THROW(RuntimeException, "gmtime_s failed ...");
        }
        t.tm_hour += 9; // GMT+9
        mktime(&t);
        ctx.infoF("%s (jk%d) %d年%02d月%02d日 %02d時%02d分%02d秒 から %d時間%02d分%02d秒",
            tvname_.c_str(), jknum_,
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
            duration / 3600, (duration / 60) % 60, duration % 60);

        if (!getNicoJKXml(startTime, duration)) return false;
        ctx.infoF("コメントXML取得: %.2f秒", sw.getAndReset());
        return nicoConvASS(CONV_ASS_XML, startTime);
    } else {
        if (setting_.isUseNicoJKLog()) return false;
        return nicoConvASS(CONV_ASS_TS, startTime);
    }
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
