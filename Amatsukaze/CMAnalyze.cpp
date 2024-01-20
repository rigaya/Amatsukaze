/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "CMAnalyze.h"

CMAnalyze::CMAnalyze(AMTContext& ctx,
    const ConfigWrapper& setting) :
    AMTObject(ctx),
    setting_(setting) {}

void CMAnalyze::analyze(const int serviceId, const int videoFileIndex, const int numFrames, const bool analyzeChapterAndCM) {

    Stopwatch sw;
    const tstring avspath = makeAVSFile(videoFileIndex);

    // ロゴ解析
    if (setting_.getLogoPath().size() > 0 || setting_.getEraseLogoPath().size() > 0) {
        analyzeLogo(videoFileIndex, sw, avspath);
    }

    // チャプター・CM解析
    if (analyzeChapterAndCM) {
        analyzeChapterCM(serviceId, videoFileIndex, numFrames, sw, avspath);
    }
}

void CMAnalyze::analyzeLogo(const int videoFileIndex, Stopwatch& sw, const tstring& avspath) {
    ctx.info("[ロゴ解析]");
    sw.start();
    logoFrame(videoFileIndex, avspath);
    ctx.infoF("完了: %.2f秒", sw.getAndReset());

    ctx.info("[ロゴ解析結果]");
    if (logopath.size() > 0) {
        ctx.infoF("マッチしたロゴ: %s", logopath);
        PrintFileAll(setting_.getTmpLogoFramePath(videoFileIndex));
    }
    const auto& eraseLogoPath = setting_.getEraseLogoPath();
    for (int i = 0; i < (int)eraseLogoPath.size(); ++i) {
        ctx.infoF("追加ロゴ%d: %s", i + 1, eraseLogoPath[i]);
        PrintFileAll(setting_.getTmpLogoFramePath(videoFileIndex, i));
    }
}

void CMAnalyze::analyzeChapterCM(const int serviceId, const int videoFileIndex, const int numFrames, Stopwatch& sw, const tstring& avspath) {
    // チャプター解析
    ctx.info("[無音・シーンチェンジ解析]");
    sw.start();
    chapterExe(videoFileIndex, avspath);
    ctx.infoF("完了: %.2f秒", sw.getAndReset());

    ctx.info("[無音・シーンチェンジ解析結果]");
    PrintFileAll(setting_.getTmpChapterExeOutPath(videoFileIndex));

    // CM推定
    ctx.info("[CM解析]");
    sw.start();
    joinLogoScp(videoFileIndex, serviceId);
    ctx.infoF("完了: %.2f秒", sw.getAndReset());

    ctx.info("[CM解析結果 - TrimAVS]");
    PrintFileAll(setting_.getTmpTrimAVSPath(videoFileIndex));
    ctx.info("[CM解析結果 - 詳細]");
    PrintFileAll(setting_.getTmpJlsPath(videoFileIndex));

    // AVSファイルからCM区間を読む
    readTrimAVS(videoFileIndex, numFrames);

    // シーンチェンジ
    readSceneChanges(videoFileIndex);

    // 分割情報
    readDiv(videoFileIndex, numFrames);

    makeCMZones(numFrames);
}

// PMT変更情報からCM追加認識
void CMAnalyze::applyPmtCut(
    int numFrames, const double* rates,
    const std::vector<int>& pidChanges) {
    if (sceneChanges.size() == 0) {
        ctx.info("シーンチェンジ情報がないためPMT変更情報をCM判定に利用できません");
    }

    ctx.info("[PMT更新CM認識]");

    int validStart = 0, validEnd = numFrames;
    std::vector<int> matchedPoints;

    // picChangesに近いsceneChangesを見つける
    for (int i = 1; i < (int)pidChanges.size(); ++i) {
        int next = (int)(std::lower_bound(
            sceneChanges.begin(), sceneChanges.end(),
            pidChanges[i]) - sceneChanges.begin());
        int prev = next;
        if (next > 0) {
            prev = next - 1;
        }
        if (next == sceneChanges.size()) {
            next = prev;
        }
        //ctx.infoF("%d,%d,%d,%d,%d", pidChanges[i], next, sceneChanges[next], prev, sceneChanges[prev]);
        int diff = std::abs(pidChanges[i] - sceneChanges[next]);
        if (diff < 30 * 2) { // 次
            matchedPoints.push_back(sceneChanges[next]);
            ctx.infoF("フレーム%dのPMT変更はフレーム%dにシーンチェンジあり",
                pidChanges[i], sceneChanges[next]);
        } else {
            diff = std::abs(pidChanges[i] - sceneChanges[prev]);
            if (diff < 30 * 2) { // 前
                matchedPoints.push_back(sceneChanges[prev]);
                ctx.infoF("フレーム%dのPMT変更はフレーム%dにシーンチェンジあり",
                    pidChanges[i], sceneChanges[prev]);
            } else {
                ctx.infoF("フレーム%dのPMT変更は付近にシーンチェンジがないため無視します", pidChanges[i]);
            }
        }
    }

    // 前後カット部分を算出
    int maxCutFrames0 = (int)(rates[0] * numFrames);
    int maxCutFrames1 = numFrames - (int)(rates[1] * numFrames);
    for (int i = 0; i < (int)matchedPoints.size(); ++i) {
        if (matchedPoints[i] < maxCutFrames0) {
            validStart = std::max(validStart, matchedPoints[i]);
        }
        if (matchedPoints[i] > maxCutFrames1) {
            validEnd = std::min(validEnd, matchedPoints[i]);
        }
    }
    ctx.infoF("設定区間: 0-%d %d-%d", maxCutFrames0, maxCutFrames1, numFrames);
    ctx.infoF("検出CM区間: 0-%d %d-%d", validStart, validEnd, numFrames);

    // trimsに反映
    auto copy = trims;
    trims.clear();
    for (int i = 0; i < (int)copy.size(); i += 2) {
        auto start = copy[i];
        auto end = copy[i + 1];
        if (end <= validStart) {
            // 開始前
            continue;
        } else if (start <= validStart) {
            // 途中から開始
            start = validStart;
        }
        if (start >= validEnd) {
            // 終了後
            continue;
        } else if (end >= validEnd) {
            // 途中で終了
            end = validEnd;
        }
        trims.push_back(start);
        trims.push_back(end);
    }

    // cmzonesに反映
    makeCMZones(numFrames);
}

void CMAnalyze::inputTrimAVS(int numFrames, const tstring& trimavsPath) {
    ctx.infoF("[Trim情報入力]: %s", trimavsPath.c_str());
    PrintFileAll(trimavsPath);

    // AVSファイルからCM区間を読む
    File file(trimavsPath, _T("r"));
    std::string str;
    if (!file.getline(str)) {
        THROW(FormatException, "TrimAVSファイルが読めません");
    }
    readTrimAVS(str, numFrames);

    // cmzonesに反映
    makeCMZones(numFrames);
}
CMAnalyze::MySubProcess::MySubProcess(const tstring& args, File* out, File* err)
    : EventBaseSubProcess(args)
    , out(out)
    , err(err) {}
/* virtual */ void CMAnalyze::MySubProcess::onOut(bool isErr, MemoryChunk mc) {
    // これはマルチスレッドで呼ばれるの注意
    File* dst = isErr ? err : out;
    if (dst != nullptr) {
        dst->write(mc);
    } else {
        fwrite(mc.data, mc.length, 1, SUBPROC_OUT);
        fflush(SUBPROC_OUT);
    }
}

tstring CMAnalyze::makeAVSFile(int videoFileIndex) {
    StringBuilder sb;

    // オートロードプラグインのロードに失敗すると動作しなくなるのでそれを回避
    sb.append("ClearAutoloadDirs()\n");

    sb.append("LoadPlugin(\"%s\")\n", GetModulePath());
    sb.append("AMTSource(\"%s\")\n", setting_.getTmpAMTSourcePath(videoFileIndex));
    sb.append("Prefetch(1)\n");
    tstring avspath = setting_.getTmpSourceAVSPath(videoFileIndex);
    File file(avspath, _T("w"));
    file.write(sb.getMC());
    return avspath;
}

std::string CMAnalyze::makePreamble() {
    StringBuilder sb;
    // システムのプラグインフォルダを無効化
    if (setting_.isSystemAvsPlugin() == false) {
        sb.append("ClearAutoloadDirs()\n");
    }
    // Amatsukaze用オートロードフォルダを追加
    sb.append("AddAutoloadDir(\"%s/plugins64\")\n", GetModuleDirectory());
    return sb.str();
}

void CMAnalyze::logoFrame(int videoFileIndex, const tstring& avspath) {
    ScriptEnvironmentPointer env = make_unique_ptr(CreateScriptEnvironment2());

    try {
        AVSValue result;
        env->Invoke("Eval", AVSValue(makePreamble().c_str()));
        env->LoadPlugin(to_string(GetModulePath()).c_str(), true, &result);
        PClip clip = env->Invoke("AMTSource", to_string(setting_.getTmpAMTSourcePath(videoFileIndex)).c_str()).AsClip();

        auto vi = clip->GetVideoInfo();
        int duration = vi.num_frames * vi.fps_denominator / vi.fps_numerator;

        const auto& logoPath = setting_.getLogoPath();
        const auto& eraseLogoPath = setting_.getEraseLogoPath();

        std::vector<tstring> allLogoPath = logoPath;
        allLogoPath.insert(allLogoPath.end(), eraseLogoPath.begin(), eraseLogoPath.end());
        logo::LogoFrame logof(ctx, allLogoPath, 0.35f);
        logof.scanFrames(clip, env.get());

        if (logoPath.size() > 0) {
#if 0
            logof.dumpResult(setting_.getTmpLogoFramePath(videoFileIndex));
#endif
            logof.selectLogo((int)logoPath.size());
            logof.writeResult(setting_.getTmpLogoFramePath(videoFileIndex));

            float threshold = setting_.isLooseLogoDetection() ? 0.03f : (duration <= 60 * 7) ? 0.03f : 0.1f;
            if (logof.getLogoRatio() < threshold) {
                ctx.info("この区間はマッチするロゴはありませんでした");
            } else {
                logopath = setting_.getLogoPath()[logof.getBestLogo()];
            }
        }

        for (int i = 0; i < (int)eraseLogoPath.size(); ++i) {
            logof.writeResult(setting_.getTmpLogoFramePath(videoFileIndex, i), (int)logoPath.size() + i);
        }
    } catch (const AvisynthError& avserror) {
        THROWF(AviSynthException, "%s", avserror.msg);
    }
}

tstring CMAnalyze::MakeChapterExeArgs(int videoFileIndex, const tstring& avspath) {
    return StringFormat(_T("\"%s\" -v \"%s\" -o \"%s\" %s"),
        setting_.getChapterExePath(), pathToOS(avspath),
        pathToOS(setting_.getTmpChapterExePath(videoFileIndex)),
        setting_.getChapterExeOptions());
}

void CMAnalyze::chapterExe(int videoFileIndex, const tstring& avspath) {
    File stdoutf(setting_.getTmpChapterExeOutPath(videoFileIndex), _T("wb"));
    auto args = MakeChapterExeArgs(videoFileIndex, avspath);
    ctx.infoF("%s", args);
    MySubProcess process(args, &stdoutf);
    int exitCode = process.join();
    if (exitCode != 0) {
        THROWF(FormatException, "ChapterExeがエラーコード(%d)を返しました", exitCode);
    }
}

tstring CMAnalyze::MakeJoinLogoScpArgs(int videoFileIndex) {
    StringBuilderT sb;
    sb.append(_T("\"%s\""), setting_.getJoinLogoScpPath());
    if (logopath.size() > 0) {
        sb.append(_T(" -inlogo \"%s\""), pathToOS(setting_.getTmpLogoFramePath(videoFileIndex)));
    }
    sb.append(_T(" -inscp \"%s\" -incmd \"%s\" -o \"%s\" -oscp \"%s\" -odiv \"%s\" %s"),
        pathToOS(setting_.getTmpChapterExePath(videoFileIndex)),
        pathToOS(setting_.getJoinLogoScpCmdPath()),
        pathToOS(setting_.getTmpTrimAVSPath(videoFileIndex)),
        pathToOS(setting_.getTmpJlsPath(videoFileIndex)),
        pathToOS(setting_.getTmpDivPath(videoFileIndex)),
        setting_.getJoinLogoScpOptions());
    return sb.str();
}

void CMAnalyze::joinLogoScp(int videoFileIndex, int serviceId) {
    auto args = MakeJoinLogoScpArgs(videoFileIndex);
    ctx.infoF("%s", args);
    // join_logo_scp向けの環境変数を設定
    const tstring clioutpath = setting_.getOutFileBaseWithoutPrefix() + _T(".") + setting_.getOutputExtention(setting_.getFormat());
    SetEnvironmentVariable(_T("CLI_IN_PATH"), setting_.getSrcFilePath().c_str());
    SetEnvironmentVariable(_T("SERVICE_ID"), StringFormat(_T("%d"), serviceId).c_str());
    SetEnvironmentVariable(_T("CLI_OUT_PATH"), clioutpath.c_str());
    ctx.infoF("CLI_IN_PATH  : %s", to_string(setting_.getSrcFilePath()).c_str());
    ctx.infoF("SERVICE_ID   : %d", serviceId);
    ctx.infoF("CLI_OUT_PATH : %s", to_string(clioutpath).c_str());
    MySubProcess process(args);
    int exitCode = process.join();
    if (exitCode != 0) {
        THROWF(FormatException, "join_logo_scp.exeがエラーコード(%d)を返しました", exitCode);
    }
    // join_logo_scp向けの環境変数を解除
    SetEnvironmentVariable(_T("CLI_IN_PATH"), NULL);
    SetEnvironmentVariable(_T("SERVICE_ID"), NULL);
    SetEnvironmentVariable(_T("CLI_OUT_PATH"), NULL);
}

void CMAnalyze::readTrimAVS(int videoFileIndex, int numFrames) {
    File file(setting_.getTmpTrimAVSPath(videoFileIndex), _T("r"));
    std::string str;
    if (!file.getline(str)) {
        THROW(FormatException, "join_logo_scp.exeの出力AVSファイルが読めません");
    }
    readTrimAVS(str, numFrames);
}

void CMAnalyze::readTrimAVS(std::string str, int numFrames) {
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    std::regex re("trim\\s*\\(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*\\)");
    std::sregex_iterator iter(str.begin(), str.end(), re);
    std::sregex_iterator end;

    trims.clear();
    for (; iter != end; ++iter) {
        trims.push_back(std::stoi((*iter)[1].str()));
        trims.push_back(std::stoi((*iter)[2].str()) + 1);
    }
}

void CMAnalyze::readDiv(int videoFileIndex, int numFrames) {
    File file(setting_.getTmpDivPath(videoFileIndex), _T("r"));
    std::string str;
    divs.clear();
    while (file.getline(str)) {
        if (str.size()) {
            divs.push_back(std::atoi(str.c_str()));
        }
    }
    // 正規化
    if (divs.size() == 0) {
        divs.push_back(0);
    }
    if (divs.front() != 0) {
        divs.insert(divs.begin(), 0);
    }
    divs.push_back(numFrames);
}

void CMAnalyze::readSceneChanges(int videoFileIndex) {
    File file(setting_.getTmpChapterExeOutPath(videoFileIndex), _T("r"));
    std::string str;

    // ヘッダ部分をスキップ
    while (1) {
        if (!file.getline(str)) {
            THROW(FormatException, "ChapterExe.exeの出力ファイルが読めません");
        }
        if (starts_with(str, "----")) {
            break;
        }
    }

    std::regex re0("mute\\s*(\\d+):\\s*(\\d+)\\s*-\\s*(\\d+).*");
    std::regex re1("\\s*SCPos:\\s*(\\d+).*");

    while (file.getline(str)) {
        std::smatch m;
        if (std::regex_search(str, m, re0)) {
            //std::stoi(m[1].str());
            //std::stoi(m[2].str());
        } else if (std::regex_search(str, m, re1)) {
            sceneChanges.push_back(std::stoi(m[1].str()));
        }
    }
}

void CMAnalyze::makeCMZones(int numFrames) {
    std::deque<int> split(trims.begin(), trims.end());
    split.push_front(0);
    split.push_back(numFrames);

    for (int i = 1; i < (int)split.size(); ++i) {
        if (split[i] < split[i - 1]) {
            THROW(FormatException, "join_logo_scp.exeの出力AVSファイルが不正です");
        }
    }

    cmzones.clear();
    for (int i = 0; i < (int)split.size(); i += 2) {
        EncoderZone zone = { split[i], split[i + 1] };
        if (zone.endFrame - zone.startFrame > 0) {
            cmzones.push_back(zone);
        }
    }
}
MakeChapter::MakeChapter(AMTContext& ctx,
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    int videoFileIndex,
    const std::vector<int>& trims)
    : AMTObject(ctx)
    , setting(setting)
    , reformInfo(reformInfo) {
    makeBase(trims, readJls(setting.getTmpJlsPath(videoFileIndex)));
}

void MakeChapter::exec(EncodeFileKey key) {
    auto filechapters = makeFileChapter(key);
    if (filechapters.size() > 0) {
        writeChapter(filechapters, key);
    }
}

std::vector<MakeChapter::JlsElement> MakeChapter::readJls(const tstring& jlspath) {
    File file(jlspath, _T("r"));
    std::regex re("^\\s*(\\d+)\\s+(\\d+)\\s+(\\d+)\\s+([-\\d]+)\\s+(\\d+).*:(\\S+)");
    std::regex reOld("^\\s*(\\d+)\\s+(\\d+)\\s+(\\d+)\\s+([-\\d]+)\\s+(\\d+)");
    std::string str;
    std::vector<JlsElement> elements;
    while (file.getline(str)) {
        std::smatch m;
        if (std::regex_search(str, m, re)) {
            JlsElement elem = {
                std::stoi(m[1].str()),
                std::stoi(m[2].str()) + 1,
                std::stoi(m[3].str()),
                m[6].str()
            };
            elements.push_back(elem);
        } else if (std::regex_search(str, m, reOld)) {
            JlsElement elem = {
                std::stoi(m[1].str()),
                std::stoi(m[2].str()) + 1,
                std::stoi(m[3].str()),
                ""
            };
            elements.push_back(elem);
        }
    }
    return elements;
}

/* static */ bool MakeChapter::startsWith(const std::string& s, const std::string& prefix) {
    auto size = prefix.size();
    if (s.size() < size) return false;
    return std::equal(std::begin(prefix), std::end(prefix), std::begin(s));
}

void MakeChapter::makeBase(std::vector<int> trims, std::vector<JlsElement> elements) {
    // isCut, isCMフラグを生成
    for (int i = 0; i < (int)elements.size(); ++i) {
        auto& e = elements[i];
        int trimIdx = (int)(std::lower_bound(trims.begin(), trims.end(), (e.frameStart + e.frameEnd) / 2) - trims.begin());
        e.isCut = !(trimIdx % 2);
        e.isCM = (e.comment == "CM");
        e.isOld = (e.comment.size() == 0);
    }

    // 余分なものはマージ
    JlsElement cur = elements[0];
    for (int i = 1; i < (int)elements.size(); ++i) {
        auto& e = elements[i];
        bool isMerge = false;
        if (cur.isCut && e.isCut) {
            if (cur.isCM == e.isCM) {
                isMerge = true;
            }
        }
        if (isMerge) {
            cur.frameEnd = e.frameEnd;
            cur.seconds += e.seconds;
        } else {
            chapters.push_back(cur);
            cur = e;
        }
    }
    chapters.push_back(cur);

    // コメントをチャプター名に変更
    int nChapter = -1;
    bool prevCM = true;
    for (int i = 0; i < (int)chapters.size(); ++i) {
        auto& c = chapters[i];
        if (c.isCut) {
            if (c.isCM || c.isOld) c.comment = "CM";
            else c.comment = "CM?";
            prevCM = true;
        } else {
            bool showSec = false;
            if (startsWith(c.comment, "Trailer") ||
                startsWith(c.comment, "Sponsor") ||
                startsWith(c.comment, "Endcard") ||
                startsWith(c.comment, "Edge") ||
                startsWith(c.comment, "Border") ||
                c.seconds == 60 ||
                c.seconds == 90) {
                showSec = true;
            }
            if (prevCM) {
                ++nChapter;
                prevCM = false;
            }
            c.comment = 'A' + (nChapter % 26);
            if (showSec) {
                c.comment += std::to_string(c.seconds) + "Sec";
            }
        }
    }
}

std::vector<MakeChapter::JlsElement> MakeChapter::makeFileChapter(EncodeFileKey key) {
    const auto& outFrames = reformInfo.getEncodeFile(key).videoFrames;

    // チャプターを分割後のフレーム番号に変換
    std::vector<JlsElement> cvtChapters;
    for (int i = 0; i < (int)chapters.size(); ++i) {
        const auto& c = chapters[i];
        JlsElement fc = c;
        fc.frameStart = (int)(std::lower_bound(outFrames.begin(), outFrames.end(), c.frameStart) - outFrames.begin());
        fc.frameEnd = (int)(std::lower_bound(outFrames.begin(), outFrames.end(), c.frameEnd) - outFrames.begin());
        cvtChapters.push_back(fc);
    }

    // 短すぎるチャプターは消す
    auto& vfmt = reformInfo.getFormat(key).videoFormat;
    int fps = (int)std::round((float)vfmt.frameRateNum / vfmt.frameRateDenom);
    std::vector<JlsElement> fileChapters;
    JlsElement cur = { 0 };
    for (int i = 0; i < (int)cvtChapters.size(); ++i) {
        auto& c = cvtChapters[i];
        if (c.frameEnd - c.frameStart < fps * 2) { // 2秒以下のチャプターは消す
            cur.frameEnd = c.frameEnd;
        } else if (cur.comment.size() == 0) {
            // まだ中身を入れていない場合は今のチャプターを入れる
            int start = cur.frameStart;
            cur = c;
            cur.frameStart = start;
        } else {
            // もう中身が入っているので、出力
            fileChapters.push_back(cur);
            cur = c;
        }
    }
    if (cur.comment.size() > 0) {
        fileChapters.push_back(cur);
    }

    return fileChapters;
}

void MakeChapter::writeChapter(const std::vector<JlsElement>& chapters, EncodeFileKey key) {
    auto& vfmt = reformInfo.getFormat(key).videoFormat;
    float frameMs = (float)vfmt.frameRateDenom / vfmt.frameRateNum * 1000.0f;

    ctx.infoF("ファイル: %d-%d-%d %s", key.video, key.format, key.div, CMTypeToString(key.cm));

    StringBuilder sb;
    int sumframes = 0;
    for (int i = 0; i < (int)chapters.size(); ++i) {
        auto& c = chapters[i];

        ctx.infoF("%5d: %s", c.frameStart, c.comment.c_str());

        int ms = (int)std::round(sumframes * frameMs);
        int s = ms / 1000;
        int m = s / 60;
        int h = m / 60;
        int ss = ms % 1000;
        s %= 60;
        m %= 60;
        h %= 60;

        sb.append("CHAPTER%02d=%02d:%02d:%02d.%03d\n", (i + 1), h, m, s, ss);
        sb.append("CHAPTER%02dNAME=%s\n", (i + 1), c.comment);

        sumframes += c.frameEnd - c.frameStart;
    }

    File file(setting.getTmpChapterPath(key), _T("w"));
    file.write(sb.getMC());
}
