#pragma once

/**
* Output frames to encoder
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "ReaderWriterFFmpeg.h"
#include "TranscodeSetting.h"
#include "FilteredSource.h"

class Y4MWriter {
    static const char* getPixelFormat(VideoInfo vi);
public:
    Y4MWriter(VideoInfo vi, VideoFormat outfmt);
    void inputFrame(const PVideoFrame& frame);
protected:
    virtual void onWrite(MemoryChunk mc) = 0;
private:
    int n;
    int nc;
    std::string header;
    std::string frameHeader;
    AutoBuffer buffer;
};

class Y4MEncodeWriter : AMTObject, NonCopyable {
    static const char* getYUV(VideoInfo vi);
public:
    Y4MEncodeWriter(AMTContext& ctx, const tstring& encoder_args, VideoInfo vi, VideoFormat fmt, bool disablePowerThrottoling);
    ~Y4MEncodeWriter();

    void inputFrame(const PVideoFrame& frame);

    void finish();

    const std::deque<std::vector<char>>& getLastLines();

private:
    class MyVideoWriter : public Y4MWriter {
    public:
        MyVideoWriter(Y4MEncodeWriter* this_, VideoInfo vi, VideoFormat fmt);
    protected:
        virtual void onWrite(MemoryChunk mc);
    private:
        Y4MEncodeWriter* this_;
    };

    std::unique_ptr<MyVideoWriter> y4mWriter_;
    std::unique_ptr<StdRedirectedSubProcess> process_;

    void onVideoWrite(MemoryChunk mc);
};

class AMTFilterVideoEncoder : public AMTObject {
public:
    AMTFilterVideoEncoder(
        AMTContext&ctx, int numEncodeBufferFrames);

    void encode(
        PClip source, VideoFormat outfmt, const std::vector<double>& timeCodes,
        const std::vector<tstring>& encoderOptions, const bool disablePowerThrottoling,
        IScriptEnvironment* env);

private:

    class SpDataPumpThread : public DataPumpThread<std::unique_ptr<PVideoFrame>, true> {
    public:
        SpDataPumpThread(AMTFilterVideoEncoder* this_, int bufferingFrames);
    protected:
        virtual void OnDataReceived(std::unique_ptr<PVideoFrame>&& data);
    private:
        AMTFilterVideoEncoder * this_;
    };

    VideoInfo vi_;
    VideoFormat outfmt_;
    std::unique_ptr<Y4MEncodeWriter> encoder_;

    SpDataPumpThread thread_;
};

class AMTSimpleVideoEncoder : public AMTObject {
public:
    AMTSimpleVideoEncoder(
        AMTContext& ctx,
        const ConfigWrapper& setting);

    void encode();

    int getAudioCount() const;

    int64_t getSrcFileSize() const;

    VideoFormat getVideoFormat() const;

private:
    class SpVideoReader : public av::VideoReader {
    public:
        SpVideoReader(AMTSimpleVideoEncoder* this_);
    protected:
        virtual void onFileOpen(AVFormatContext *fmt);
        virtual void onVideoFormat(AVStream *stream, VideoFormat fmt);
        virtual void onFrameDecoded(av::Frame& frame);
        virtual void onAudioPacket(AVPacket& packet);
    private:
        AMTSimpleVideoEncoder * this_;
    };

    class SpDataPumpThread : public DataPumpThread<std::unique_ptr<av::Frame>> {
    public:
        SpDataPumpThread(AMTSimpleVideoEncoder* this_, int bufferingFrames);
    protected:
        virtual void OnDataReceived(std::unique_ptr<av::Frame>&& data);
    private:
        AMTSimpleVideoEncoder * this_;
    };

    class AudioFileWriter : public av::AudioWriter {
    public:
        AudioFileWriter(AVStream* stream, const tstring& filename, int bufsize);
    protected:
        virtual void onWrite(MemoryChunk mc);
    private:
        File file_;
    };

    const ConfigWrapper& setting_;
    SpVideoReader reader_;
    av::EncodeWriter* encoder_;
    SpDataPumpThread thread_;

    int audioCount_;
    std::vector<std::unique_ptr<AudioFileWriter>> audioFiles_;
    std::vector<int> audioMap_;

    int64_t srcFileSize_;
    VideoFormat videoFormat_;
    RFFExtractor rffExtractor_;

    int pass_;

    void onFileOpen(AVFormatContext *fmt);

    void processAllData(int pass);

    void onVideoFormat(AVStream *stream, VideoFormat fmt);

    void onFrameDecoded(av::Frame& frame__);

    void onFrameReceived(std::unique_ptr<av::Frame>&& frame);

    void onAudioPacket(AVPacket& packet);
};

