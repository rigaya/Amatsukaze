#pragma once

/**
* Transcode manager
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <string>
#include <memory>
#include <limits>
#include <smmintrin.h>

#include "TsSplitter.h"
#include "Encoder.h"
#include "Muxer.h"
#include "StreamReform.h"
#include "LogoScan.h"
#include "CMAnalyze.h"
#include "InterProcessComm.h"
#include "CaptionData.h"
#include "CaptionFormatter.h"
#include "EncoderOptionParser.h"
#include "NicoJK.h"
#include "AudioEncoder.h"

class AMTSplitter : public TsSplitter {
public:
    AMTSplitter(AMTContext& ctx, const ConfigWrapper& setting);

    StreamReformInfo split();

    int64_t getSrcFileSize() const;

    int64_t getTotalIntVideoSize() const;

protected:
    class StreamFileWriteHandler : public PsStreamWriter::EventHandler {
        TsSplitter& this_;
        std::unique_ptr<File> file_;
        int64_t totalIntVideoSize_;
    public:
        StreamFileWriteHandler(TsSplitter& this_);
        virtual void onStreamData(MemoryChunk mc);
        void open(const tstring& path);
        void close();
        int64_t getTotalSize() const;
    };

    const ConfigWrapper& setting_;
    PsStreamWriter psWriter;
    StreamFileWriteHandler writeHandler;
    File audioFile_;
    File waveFile_;
    VideoFormat curVideoFormat_;

    int videoFileCount_;
    int videoStreamType_;
    int audioStreamType_;
    int64_t audioFileSize_;
    int64_t waveFileSize_;
    int64_t srcFileSize_;

    // データ
    std::vector<FileVideoFrameInfo> videoFrameList_;
    std::vector<FileAudioFrameInfo> audioFrameList_;
    std::vector<StreamEvent> streamEventList_;
    std::vector<CaptionItem> captionTextList_;
    std::vector<std::pair<int64_t, JSTTime>> timeList_;

    void readAll();

    static bool CheckPullDown(PICTURE_TYPE p0, PICTURE_TYPE p1);

    void printInteraceCount();

    // TsSplitter仮想関数 //

    virtual void onVideoPesPacket(
        int64_t clock,
        const std::vector<VideoFrameInfo>& frames,
        PESPacket packet);

    virtual void onVideoFormatChanged(VideoFormat fmt);

    virtual void onAudioPesPacket(
        int audioIdx,
        int64_t clock,
        const std::vector<AudioFrameData>& frames,
        PESPacket packet);

    virtual void onAudioFormatChanged(int audioIdx, AudioFormat fmt);

    virtual void onCaptionPesPacket(
        int64_t clock,
        std::vector<CaptionItem>& captions,
        PESPacket packet);

    virtual DRCSOutInfo getDRCSOutPath(int64_t PTS, const std::string& md5);

    // TsPacketSelectorHandler仮想関数 //

    virtual void onPidTableChanged(const PMTESInfo video, const std::vector<PMTESInfo>& audio, const PMTESInfo caption);

    virtual void onTime(int64_t clock, JSTTime time);
};


static tstring replaceOptions(
    const tstring& options,
    const VideoFormat& fmt,
    const ConfigWrapper& setting,
    const EncodeFileKey key,
    const int serviceID);

class EncoderArgumentGenerator {
public:
    EncoderArgumentGenerator(
        const ConfigWrapper& setting,
        StreamReformInfo& reformInfo);

    tstring GenEncoderOptions(
        int numFrames,
        VideoFormat outfmt,
        std::vector<BitrateZone> zones,
        double vfrBitrateScale,
        tstring timecodepath,
        int vfrTimingFps,
        EncodeFileKey key, int pass, int serviceID,
        const EncoderOptionInfo& eoInfo);

    // src, target
    std::pair<double, double> printBitrate(AMTContext& ctx, EncodeFileKey key) const;

private:
    const ConfigWrapper& setting_;
    const StreamReformInfo& reformInfo_;

    double getSourceBitrate(int fileId) const;
};

std::vector<BitrateZone> MakeBitrateZones(
    const std::vector<double>& timeCodes,
    const std::vector<EncoderZone>& cmzones,
    const ConfigWrapper& setting,
    const EncoderOptionInfo& eoInfo,
    VideoInfo outvi);

#if 0
// ページヒープが機能しているかテスト
void DoBadThing();
#endif

void transcodeMain(AMTContext& ctx, const ConfigWrapper& setting);

void transcodeSimpleMain(AMTContext& ctx, const ConfigWrapper& setting);


class DrcsSearchSplitter : public TsSplitter {
public:
    DrcsSearchSplitter(AMTContext& ctx, const ConfigWrapper& setting);

    void readAll();

protected:
    const ConfigWrapper& setting_;
    std::vector<VideoFrameInfo> videoFrameList_;

    // TsSplitter仮想関数 //

    virtual void onVideoPesPacket(
        int64_t clock,
        const std::vector<VideoFrameInfo>& frames,
        PESPacket packet);

    virtual void onVideoFormatChanged(VideoFormat fmt);

    virtual void onAudioPesPacket(
        int audioIdx,
        int64_t clock,
        const std::vector<AudioFrameData>& frames,
        PESPacket packet);

    virtual void onAudioFormatChanged(int audioIdx, AudioFormat fmt);

    virtual void onCaptionPesPacket(
        int64_t clock,
        std::vector<CaptionItem>& captions,
        PESPacket packet);

    virtual DRCSOutInfo getDRCSOutPath(int64_t PTS, const std::string& md5);

    virtual void onTime(int64_t clock, JSTTime time);
};

class SubtitleDetectorSplitter : public TsSplitter {
public:
    SubtitleDetectorSplitter(AMTContext& ctx, const ConfigWrapper& setting);

    void readAll(int maxframes);

    bool getHasSubtitle() const;

protected:
    const ConfigWrapper& setting_;
    std::vector<VideoFrameInfo> videoFrameList_;
    bool hasSubtltle_;

    // TsSplitter仮想関数 //

    virtual void onVideoPesPacket(
        int64_t clock,
        const std::vector<VideoFrameInfo>& frames,
        PESPacket packet);

    virtual void onVideoFormatChanged(VideoFormat fmt);

    virtual void onAudioPesPacket(
        int audioIdx,
        int64_t clock,
        const std::vector<AudioFrameData>& frames,
        PESPacket packet);

    virtual void onAudioFormatChanged(int audioIdx, AudioFormat fmt);

    virtual void onCaptionPesPacket(
        int64_t clock,
        std::vector<CaptionItem>& captions,
        PESPacket packet);

    virtual DRCSOutInfo getDRCSOutPath(int64_t PTS, const std::string& md5);

    virtual void onTime(int64_t clock, JSTTime time);

    virtual void onCaptionPacket(int64_t clock, TsPacket packet);
};

class AudioDetectorSplitter : public TsSplitter {
public:
    AudioDetectorSplitter(AMTContext& ctx, const ConfigWrapper& setting);

    void readAll(int maxframes);

protected:
    const ConfigWrapper& setting_;
    std::vector<VideoFrameInfo> videoFrameList_;

    // TsSplitter仮想関数 //

    virtual void onVideoPesPacket(
        int64_t clock,
        const std::vector<VideoFrameInfo>& frames,
        PESPacket packet);

    virtual void onVideoFormatChanged(VideoFormat fmt);

    virtual void onAudioPesPacket(
        int audioIdx,
        int64_t clock,
        const std::vector<AudioFrameData>& frames,
        PESPacket packet);

    virtual void onAudioFormatChanged(int audioIdx, AudioFormat fmt);

    virtual void onCaptionPesPacket(
        int64_t clock,
        std::vector<CaptionItem>& captions,
        PESPacket packet);

    virtual DRCSOutInfo getDRCSOutPath(int64_t PTS, const std::string& md5);

    virtual void onTime(int64_t clock, JSTTime time);
};

void searchDrcsMain(AMTContext& ctx, const ConfigWrapper& setting);

void detectSubtitleMain(AMTContext& ctx, const ConfigWrapper& setting);

void detectAudioMain(AMTContext& ctx, const ConfigWrapper& setting);

