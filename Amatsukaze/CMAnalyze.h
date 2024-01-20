#pragma once

/**
* Amtasukaze CM Analyze
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <fstream>
#include <string>
#include <iostream>
#include <memory>
#include <regex>

#include "StreamUtils.h"
#include "TranscodeSetting.h"
#include "LogoScan.h"
#include "ProcessThread.h"
#include "PerformanceUtil.h"
#include "StreamReform.h"

class CMAnalyze : public AMTObject {
public:
    CMAnalyze(AMTContext& ctx,
        const ConfigWrapper& setting);

    void analyze(const int serviceId, const int videoFileIndex, const int numFrames, const bool analyzeChapterAndCM);

    const tstring& getLogoPath() const { return logopath; }
    const std::vector<int>& getTrims() const { return trims; }
    const std::vector<EncoderZone>& getZones() const { return cmzones; }
    const std::vector<int>& getDivs() const { return divs; }

    // PMTïœçXèÓïÒÇ©ÇÁCMí«â¡îFéØ
    void applyPmtCut(
        int numFrames, const double* rates,
        const std::vector<int>& pidChanges);

    void inputTrimAVS(int numFrames, const tstring& trimavsPath);

private:
    class MySubProcess : public EventBaseSubProcess {
    public:
        MySubProcess(const tstring& args, File* out = nullptr, File* err = nullptr);
    protected:
        File* out;
        File* err;
        virtual void onOut(bool isErr, MemoryChunk mc);
    };

    const ConfigWrapper& setting_;

    bool logoAnalysisDone;
    tstring logopath;
    std::vector<int> trims;
    std::vector<EncoderZone> cmzones;
    std::vector<int> sceneChanges;
    std::vector<int> divs;

    void analyzeLogo(const int videoFileIndex, Stopwatch& sw, const tstring& avspath);

    void analyzeChapterCM(const int serviceId, const int videoFileIndex, const int numFrames, Stopwatch& sw, const tstring& avspath);

    tstring makeAVSFile(int videoFileIndex);

    std::string makePreamble();

    void logoFrame(int videoFileIndex, const tstring& avspath);

    tstring MakeChapterExeArgs(int videoFileIndex, const tstring& avspath);

    void chapterExe(int videoFileIndex, const tstring& avspath);

    tstring MakeJoinLogoScpArgs(int videoFileIndex);

    void joinLogoScp(int videoFileIndex, int serviceId);

    void readTrimAVS(int videoFileIndex, int numFrames);

    void readTrimAVS(std::string str, int numFrames);

    void readDiv(int videoFileIndex, int numFrames);

    bool logoOffInJL(const int videoFileIndex) const;

    void readSceneChanges(int videoFileIndex);

    void makeCMZones(int numFrames);
};

class MakeChapter : public AMTObject {
public:
    MakeChapter(AMTContext& ctx,
        const ConfigWrapper& setting,
        const StreamReformInfo& reformInfo,
        int videoFileIndex,
        const std::vector<int>& trims);

    void exec(EncodeFileKey key);

private:
    struct JlsElement {
        int frameStart;
        int frameEnd;
        int seconds;
        std::string comment;
        bool isCut;
        bool isCM;
        bool isOld;
    };

    const ConfigWrapper& setting;
    const StreamReformInfo& reformInfo;

    std::vector<JlsElement> chapters;

    std::vector<JlsElement> readJls(const tstring& jlspath);

    static bool startsWith(const std::string& s, const std::string& prefix);

    void makeBase(std::vector<int> trims, std::vector<JlsElement> elements);

    std::vector<JlsElement> makeFileChapter(EncodeFileKey key);

    void writeChapter(const std::vector<JlsElement>& chapters, EncodeFileKey key);
};

