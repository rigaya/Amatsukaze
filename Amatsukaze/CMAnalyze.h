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

class SetTemporaryEnvironmentVariable {
private:
    std::vector<tstring> varnames;
public:
    SetTemporaryEnvironmentVariable() : varnames() {
    }
#if defined(_WIN32) || defined(_WIN64)
    ~SetTemporaryEnvironmentVariable() {
        for (const auto& name : varnames) {
            SetEnvironmentVariable(name.c_str(), nullptr);
        }
    }
    bool set(const tstring& name, const tstring& value) {
        varnames.push_back(name);
        return SetEnvironmentVariable(name.c_str(), value.c_str());
    }
#else
    ~SetTemporaryEnvironmentVariable() {
        for (const auto& name : varnames) {
            unsetenv(name.c_str());
        }
    }
    bool set(const std::string& name, const std::string& value) {
        varnames.push_back(name);
        // setenvの第3引数は上書きを許可するかどうか（1で許可）
        return (setenv(name.c_str(), value.c_str(), 1) == 0);
    }
#endif
};

class CMAnalyze : public AMTObject {
public:
    CMAnalyze(AMTContext& ctx,
        const ConfigWrapper& setting);

    void analyze(const int serviceId, const int videoFileIndex, const VideoFormat& inputFormat, const int numFrames, const bool analyzeChapterAndCM);

    const tstring& getLogoPath() const { return logopath; }
    const std::vector<int>& getTrims() const { return trims; }
    const std::vector<EncoderZone>& getZones() const { return cmzones; }
    const std::vector<int>& getDivs() const { return divs; }

    // PMT変更情報からCM追加認識
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

    void analyzeLogo(const int videoFileIndex, const VideoFormat& inputFormat, const int numFrames, Stopwatch& sw, const tstring& avspath);

    void analyzeChapterCM(const int serviceId, const int videoFileIndex, const VideoFormat& inputFormat, const int numFrames, Stopwatch& sw, const tstring& avspath);

    tstring makeAVSFile(int videoFileIndex, const VideoFormat& inputFormat, const bool forChapterExe);

    std::string makePreamble();

    void logoFrame(const int videoFileIndex, const VideoFormat& inputFormat, const int numFrames, const tstring& avspath);

    tstring MakeChapterExeArgs(int videoFileIndex, const VideoFormat& inputFormat, const tstring& avspath);

    void chapterExe(int videoFileIndex, const VideoFormat& inputFormat, const tstring& avspath);

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

