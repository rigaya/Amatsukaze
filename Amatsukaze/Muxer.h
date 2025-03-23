#pragma once

/**
* Call muxer
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "StreamUtils.h"
#include "TranscodeSetting.h"
#include "StreamReform.h"
#include "EncoderOptionParser.h"
#include "AdtsParser.h"
#include "ProcessThread.h"

struct EncodeFileOutput {
    VideoFormat vfmt;
    std::vector<tstring> outSubs; // 外部ファイルで出力された字幕
    int64_t fileSize;
    double srcBitrate;
    double targetBitrate;
    int vfrTimingFps;
    tstring timecode;
};

ENUM_FORMAT getActualOutputFormat(EncodeFileKey key, const StreamReformInfo& reformInfo, const ConfigWrapper& setting);

class SpDualMonoSplitter : public DualMonoSplitter {
    std::unique_ptr<File> file[2];
public:
    SpDualMonoSplitter(AMTContext& ctx);
    void open(int index, const tstring& filename);
    virtual void OnOutFrame(int index, MemoryChunk mc);
};

class AMTMuxder : public AMTObject {
public:
    AMTMuxder(
        AMTContext&ctx,
        const ConfigWrapper& setting,
        const StreamReformInfo& reformInfo);

    void mux(EncodeFileKey key,
        const EncoderOptionInfo& eoInfo, // エンコーダオプション情報
        bool nicoOK,
        EncodeFileOutput& fileOut); // 出力情報

private:
    const ConfigWrapper& setting_;
    const StreamReformInfo& reformInfo_;
};

class AMTSimpleMuxder : public AMTObject {
public:
    AMTSimpleMuxder(
        AMTContext&ctx,
        const ConfigWrapper& setting);

    void mux(VideoFormat videoFormat, int audioCount);

    int64_t getTotalOutSize() const;

private:
    class MySubProcess : public EventBaseSubProcess {
    public:
        MySubProcess(const tstring& args);
    protected:
        virtual void onOut(bool isErr, MemoryChunk mc);
    };

    const ConfigWrapper& setting_;
    int64_t totalOutSize_;
};

