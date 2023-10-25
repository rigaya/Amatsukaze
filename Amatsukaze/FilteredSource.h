#pragma once

/**
* Create encoder source with avisynth filters
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <memory>
#include <numeric>
#include <regex>

#include "StreamUtils.h"
#include "ReaderWriterFFmpeg.h"
#include "TranscodeSetting.h"
#include "StreamReform.h"
#include "AMTSource.h"
#include "InterProcessComm.h"

// Defined in ComputeKernel.cpp
bool IsAVXAvailable();
bool IsAVX2Available();

class RFFExtractor {
public:
    void clear();

    void inputFrame(av::EncodeWriter& encoder, std::unique_ptr<av::Frame>&& frame, PICTURE_TYPE pic);

private:
    std::unique_ptr<av::Frame> prevFrame_;

    // 2つのフレームのトップフィールド、ボトムフィールドを合成
    static std::unique_ptr<av::Frame> mixFields(av::Frame& topframe, av::Frame& bottomframe);
};

PICTURE_TYPE getPictureTypeFromAVFrame(AVFrame* frame);

class AMTFilterSource : public AMTObject {
    class AvsScript {
    public:
        StringBuilder& Get();
        void Apply(IScriptEnvironment* env);
        void Clear();
        const std::string& Str() const;
    private:
        std::string script;
        StringBuilder append;
    };

    void readTimecodeFile(const tstring& filepath);

    void readTimecode(EncodeFileKey key);

public:
    // Main (+ Post)
    AMTFilterSource(AMTContext&ctx,
        const ConfigWrapper& setting,
        const StreamReformInfo& reformInfo,
        const std::vector<EncoderZone>& zones,
        const tstring& logopath,
        EncodeFileKey key,
        const ResourceManger& rm);

    ~AMTFilterSource();

    const PClip& getClip() const;

    std::string getScript() const;

    const VideoFormat& getFormat() const;

    // 入力ゾーンのtrim後のゾーンを返す
    const std::vector<EncoderZone> getZones() const;

    // 各フレームの時間ms(最後のフレームの表示時間を定義するため要素数はフレーム数+1)
    const std::vector<double>& getTimeCodes() const;

    int getVfrTimingFps() const;

    IScriptEnvironment2* getEnv() const;

private:
    const ConfigWrapper& setting_;
    ScriptEnvironmentPointer env_;
    AvsScript script_;
    PClip filter_;
    VideoFormat outfmt_;
    std::vector<EncoderZone> outZones_;
    std::vector<double> timeCodes_;
    int vfrTimingFps_;

    void writeScriptFile(EncodeFileKey key);

    std::vector<tstring> GetSuitablePlugins(const tstring& basepath);

    void InitEnv();

    void ReadAllFrames(int pass);

    void defineMakeSource(
        EncodeFileKey key,
        const StreamReformInfo& reformInfo,
        const tstring& logopath);

    void trimInput(EncodeFileKey key,
        const StreamReformInfo& reformInfo);

    // 戻り値: 前処理？
    bool FilterPass(int pass, int gpuIndex,
        EncodeFileKey key,
        const StreamReformInfo& reformInfo,
        const tstring& logopath);

    void MakeZones(
        EncodeFileKey key,
        const std::vector<EncoderZone>& zones,
        const StreamReformInfo& reformInfo);

    void MakeOutFormat(const VideoFormat& infmt);
};

class AMTDecimate : public GenericVideoFilter {
    std::vector<int> durations;
    std::vector<int> framesMap;
public:
    AMTDecimate(PClip source, const std::string& duration, IScriptEnvironment* env);

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env);

    static AVSValue __cdecl Create(AVSValue args, void* user_data, IScriptEnvironment* env);
};

// VFRでだいたいのレートコントロールを実現する
// VFRタイミングとCMゾーンからゾーンとビットレートを作成
std::vector<BitrateZone> MakeVFRBitrateZones(const std::vector<double>& timeCodes,
    const std::vector<EncoderZone>& cmzones, double bitrateCM,
    int fpsNum, int fpsDenom, double timeFactor, double costLimit);

// VFRに対応していないエンコーダでビットレート指定を行うとき用の
// 平均フレームレートを考慮したビットレートを計算する
double AdjustVFRBitrate(const std::vector<double>& timeCodes, int fpsNum, int fpsDenom);

AVSValue __cdecl AMTExec(AVSValue args, void* user_data, IScriptEnvironment* env);

class AMTOrderedParallel : public GenericVideoFilter {
    struct ClipData {
        PClip clip;
        int numFrames;
        int current;
        std::mutex mutex;
    };
    std::vector<ClipData> clips_;
public:
    AMTOrderedParallel(AVSValue clips, IScriptEnvironment* env);

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env);

    int __stdcall SetCacheHints(int cachehints, int frame_range);;

    static AVSValue __cdecl Create(AVSValue args, void* user_data, IScriptEnvironment* env);
};

