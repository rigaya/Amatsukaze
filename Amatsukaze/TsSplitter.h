#pragma once

/**
* MPEG2-TS splitter
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "common.h"

#include <algorithm>
#include <vector>
#include <map>
#include <array>

#include "StreamUtils.h"
#include "Mpeg2TsParser.h"
#include "Mpeg2VideoParser.h"
#include "H264VideoParser.h"
#include "HEVCVideoParser.h"
#include "AdtsParser.h"
#include "Mpeg2PsWriter.hpp"
#include "WaveWriter.h"
#include "CaptionDef.h"
#include "Caption.h"
#include "CaptionData.h"

class VideoFrameParser : public AMTObject, public PesParser {
public:
    VideoFrameParser(AMTContext&ctx);

    void setStreamFormat(VIDEO_STREAM_FORMAT streamFormat);

    VIDEO_STREAM_FORMAT getStreamFormat();

    void reset();

protected:

    virtual void onPesPacket(int64_t clock, PESPacket packet);

    virtual void onVideoPesPacket(int64_t clock, const std::vector<VideoFrameInfo>& frames, PESPacket packet) = 0;

    virtual void onVideoFormatChanged(VideoFormat fmt) = 0;

private:
    VIDEO_STREAM_FORMAT videoStreamFormat;
    VideoFormat videoFormat;

    std::vector<VideoFrameInfo> frameInfo;

    MPEG2VideoParser mpeg2parser;
    H264VideoParser h264parser;
    HEVCVideoParser hevcparser;

    IVideoParser* parser;

};

class AudioFrameParser : public AMTObject, public PesParser {
public:
    AudioFrameParser(AMTContext&ctx);

    virtual void onPesPacket(int64_t clock, PESPacket packet);

    virtual void onAudioPesPacket(int64_t clock, const std::vector<AudioFrameData>& frames, PESPacket packet) = 0;

    virtual void onAudioFormatChanged(AudioFormat fmt) = 0;

private:
    AudioFormat format;

    std::vector<AudioFrameData> frameData;

    AdtsParser adtsParser;
};

// 同期型の字幕のみ対応。文字スーパーには対応しない
class CaptionParser : public AMTObject, public PesParser {
public:
    CaptionParser(AMTContext&ctx);

    virtual void onPesPacket(int64_t clock, PESPacket packet);

    virtual void onCaptionPesPacket(int64_t clock, std::vector<CaptionItem>& captions, PESPacket packet) = 0;

    virtual DRCSOutInfo getDRCSOutPath(int64_t PTS, const std::string& md5) = 0;

private:
    class SpCaptionFormatter : public CaptionDLLParser {
        CaptionParser& this_;
    public:
        SpCaptionFormatter(CaptionParser& this_);
        virtual DRCSOutInfo getDRCSOutPath(int64_t PTS, const std::string& md5);
    };
    std::vector<CaptionItem> captions;
    SpCaptionFormatter fomatter;
};

// TSストリームを一定量だけ戻れるようにする
class TsPacketBuffer : public TsPacketParser {
public:
    TsPacketBuffer(AMTContext& ctx);

    void setHandler(TsPacketHandler* handler);

    int numBefferedPackets();

    void clearBuffer();

    void setEnableBuffering(bool enable);

    void setNumBufferingPackets(int numPackets);

    void backAndInput();

    virtual void onTsPacket(TsPacket packet);

private:
    TsPacketHandler* handler;
    AutoBuffer buffer;
    int numBefferedPackets_;
    int numMaxPackets;
    bool buffering;
};

class TsSystemClock : public AMTObject {
public:
    TsSystemClock(AMTContext& ctx);

    void setPcrPid(int PcrPid);

    // 十分な数のPCRを受信したか
    bool pcrReceived();

    // 現在入力されたパケットを基準にしてrelativeだけ後のパケットの入力時刻を返す
    int64_t getClock(int relative);

    // TSストリームを最初から読み直すときに呼び出す
    void backTs();

    // TSストリームの全データを入れること
    void inputTsPacket(TsPacket packet);

    double currentBitrate();

private:
    struct PCR_Info {
        int64_t clock = 0;
        int packetIndex = -1;
    };

    int PcrPid;
    int numPcrReceived;
    int numTotakPacketsReveived;
    PCR_Info pcrInfo[2];
};

class TsSplitter : public AMTObject, protected TsPacketSelectorHandler {
public:
    TsSplitter(AMTContext& ctx, bool enableVideo, bool enableAudio, bool enableCaption);

    void reset();

    // 0以下で指定無効
    void setServiceId(int sid);

    int getActualServiceId();

    void inputTsData(MemoryChunk data);
    void flush();

    int64_t getNumTotalPackets() const;

    int64_t getNumScramblePackets() const;

protected:
    enum INITIALIZATION_PHASE {
        PMT_WAITING,	// PAT,PMT待ち
        PCR_WAITING,	// ビットレート取得のためPCR2つを受信中
        INIT_FINISHED,	// 必要な情報は揃った
    };

    class SpTsPacketHandler : public TsPacketHandler {
        TsSplitter& this_;
    public:
        SpTsPacketHandler(TsSplitter& this_);

        virtual void onTsPacket(int64_t clock, TsPacket packet);
    };
    class PcrDetectionHandler : public TsPacketHandler {
        TsSplitter& this_;
    public:
        PcrDetectionHandler(TsSplitter& this_);

        virtual void onTsPacket(int64_t clock, TsPacket packet);
    };
    class SpVideoFrameParser : public VideoFrameParser {
        TsSplitter& this_;
    public:
        SpVideoFrameParser(AMTContext&ctx, TsSplitter& this_);

    protected:
        virtual void onVideoPesPacket(int64_t clock, const std::vector<VideoFrameInfo>& frames, PESPacket packet);

        virtual void onVideoFormatChanged(VideoFormat fmt);
    };
    class SpAudioFrameParser : public AudioFrameParser {
        TsSplitter& this_;
        int audioIdx;
    public:
        SpAudioFrameParser(AMTContext&ctx, TsSplitter& this_, int audioIdx);

    protected:
        virtual void onAudioPesPacket(int64_t clock, const std::vector<AudioFrameData>& frames, PESPacket packet);

        virtual void onAudioFormatChanged(AudioFormat fmt);
    };
    class SpCaptionParser : public CaptionParser {
        TsSplitter& this_;
    public:
        SpCaptionParser(AMTContext&ctx, TsSplitter& this_);

    protected:
        virtual void onCaptionPesPacket(int64_t clock, std::vector<CaptionItem>& captions, PESPacket packet);

        virtual DRCSOutInfo getDRCSOutPath(int64_t PTS, const std::string& md5);
    };

    INITIALIZATION_PHASE initPhase;

    TsPacketBuffer tsPacketParser;
    TsSystemClock tsSystemClock;
    SpTsPacketHandler tsPacketHandler;
    PcrDetectionHandler pcrDetectionHandler;
    TsPacketSelector tsPacketSelector;

    SpVideoFrameParser videoParser;
    std::vector<SpAudioFrameParser*> audioParsers;
    SpCaptionParser captionParser;

    bool enableVideo;
    bool enableAudio;
    bool enableCaption;
    int preferedServiceId;
    int selectedServiceId;

    int64_t numTotalPackets;
    int64_t numScramblePackets;

    virtual void onVideoPesPacket(
        int64_t clock,
        const std::vector<VideoFrameInfo>& frames,
        PESPacket packet) = 0;

    virtual void onVideoFormatChanged(VideoFormat fmt) = 0;

    virtual void onAudioPesPacket(
        int audioIdx,
        int64_t clock,
        const std::vector<AudioFrameData>& frames,
        PESPacket packet) = 0;

    virtual void onAudioFormatChanged(int audioIdx, AudioFormat fmt) = 0;

    virtual void onCaptionPesPacket(
        int64_t clock,
        std::vector<CaptionItem>& captions,
        PESPacket packet) = 0;

    virtual DRCSOutInfo getDRCSOutPath(int64_t PTS, const std::string& md5) = 0;

    // サービスを設定する場合はサービスのpids上でのインデックス
    // なにもしない場合は負の値の返す
    virtual int onPidSelect(int TSID, const std::vector<int>& pids);

    virtual void onPmtUpdated(int PcrPid);

    // TsPacketSelectorでPID Tableが変更された時変更後の情報が送られる
    virtual void onPidTableChanged(const PMTESInfo video, const std::vector<PMTESInfo>& audio, const PMTESInfo caption);

    bool checkScramble(TsPacket packet);

    virtual void onVideoPacket(int64_t clock, TsPacket packet);

    virtual void onAudioPacket(int64_t clock, TsPacket packet, int audioIdx);

    virtual void onCaptionPacket(int64_t clock, TsPacket packet);
};


