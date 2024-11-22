#pragma once

/**
* Reader/Writer with FFmpeg
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

#include "StreamUtils.h"
#include "ProcessThread.h"

// AMTSourceのフィルターオプションを有効にするか
#define ENABLE_FFMPEG_FILTER 0

// libffmpeg
#pragma warning(push)
#pragma warning(disable:4819) //C4819: ファイルは、現在のコード ページ (932) で表示できない文字を含んでいます。データの損失を防ぐために、ファイルを Unicode 形式で保存してください。
extern "C" {
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#if ENABLE_FFMPEG_FILTER
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#endif
}
#pragma warning(pop)
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#if ENABLE_FFMPEG_FILTER
#pragma comment(lib, "avfilter.lib")
#endif

namespace av {

int GetFFmpegThreads(int preferred, int height);

AVStream* GetVideoStream(AVFormatContext* pCtx);

AVStream* GetVideoStream(AVFormatContext* ctx, int serviceid);

class Frame {
public:
    Frame();
    Frame(const Frame& src);
    ~Frame();
    AVFrame* operator()();
    const AVFrame* operator()() const;
    Frame& operator=(const Frame& src);
private:
    AVFrame* frame_;
};

class CodecContext : NonCopyable {
public:
    CodecContext(const AVCodec* pCodec);
    CodecContext();
    ~CodecContext();
    void Set(const AVCodec* pCodec);
    void Free();
    AVCodecContext* operator()();
private:
    AVCodecContext *ctx_;
};

class InputContext : NonCopyable {
public:
    InputContext(const tstring& src);
    ~InputContext();
    AVFormatContext* operator()();
private:
    AVFormatContext* ctx_;
};

#if ENABLE_FFMPEG_FILTER
class FilterGraph : NonCopyable {
public:
    FilterGraph();
    ~FilterGraph();
    void Create();
    void Free();
    AVFilterGraph* operator()();
private:
    AVFilterGraph* ctx_;
};

class FilterInOut : NonCopyable {
public:
    FilterInOut();
    ~FilterInOut();
    AVFilterInOut*& operator()();
private:
    AVFilterInOut* ctx_;
};
#endif

class WriteIOContext : NonCopyable {
public:
    WriteIOContext(int bufsize);
    ~WriteIOContext();
    AVIOContext* operator()();
protected:
    virtual void onWrite(MemoryChunk mc) = 0;
private:
    AVIOContext* ctx_;
    static int write_packet_(void *opaque, uint8_t *buf, int buf_size);
};

class OutputContext : NonCopyable {
public:
    OutputContext(WriteIOContext& ioCtx, const char* format);
    ~OutputContext();
    AVFormatContext* operator()();
private:
    AVFormatContext* ctx_;
};

class VideoReader : AMTObject {
public:
    VideoReader(AMTContext& ctx);

    void readAll(const tstring& src, const DecoderSetting& decoderSetting);

protected:
    virtual void onFileOpen(AVFormatContext *fmt);;
    virtual void onVideoFormat(AVStream *stream, VideoFormat fmt);;
    virtual void onFrameDecoded(Frame& frame);;
    virtual void onAudioPacket(AVPacket& packet);;

private:
    VideoFormat fmt_;
    bool fieldMode_;
    std::unique_ptr<av::Frame> prevFrame_;

    const AVCodec* getHWAccelCodec(AVCodecID vcodecId, const DecoderSetting& decoderSetting);

    void onFrame(Frame& frame);

    void onFirstFrame(AVStream *stream, AVFrame *frame);

    // 2つのフレームのトップフィールド、ボトムフィールドを合成
    static std::unique_ptr<av::Frame> mergeFields(av::Frame& topframe, av::Frame& bottomframe);
};

class VideoWriter : NonCopyable {
public:
    VideoWriter(VideoFormat fmt, int bufsize);

    void inputFrame(Frame& frame);

    void flush();

    int getFrameCount() const;

    AVRational getAvgFrameRate() const;

protected:
    virtual void onWrite(MemoryChunk mc) = 0;

private:
    class TransWriteContext : public WriteIOContext {
    public:
        TransWriteContext(VideoWriter* this_, int bufsize);
    protected:
        virtual void onWrite(MemoryChunk mc);
    private:
        VideoWriter* this_;
    };

    TransWriteContext ioCtx_;
    OutputContext outputCtx_;
    CodecContext codecCtx_;
    VideoFormat fmt_;

    bool initialized_;
    int frameCount_;

    void init(Frame& frame);
};

class AudioWriter : NonCopyable {
public:
    AudioWriter(AVStream* src, int bufsize);

    void inputFrame(AVPacket& frame);

    void flush();

protected:
    virtual void onWrite(MemoryChunk mc) = 0;

private:
    class TransWriteContext : public WriteIOContext {
    public:
        TransWriteContext(AudioWriter* this_, int bufsize);
    protected:
        virtual void onWrite(MemoryChunk mc);
    private:
        AudioWriter* this_;
    };

    TransWriteContext ioCtx_;
    OutputContext outputCtx_;

    int frameCount_;
};

class Y4MParser {
public:
    void clear();
    void inputData(MemoryChunk mc);
    int getFrameCount() const;
private:
    int y4mcur;
    int frameSize;
    int frameCount;
    AutoBuffer y4mbuf;
};

class EncodeWriter : AMTObject, NonCopyable {
public:
    EncodeWriter(AMTContext& ctx);
    ~EncodeWriter();

    void start(const tstring& encoder_args, VideoFormat fmt, bool fieldMode, int bufsize);

    void inputFrame(Frame& frame);

    void finish();

    int getFrameCount() const;

    AVRational getFrameRate() const;

    const std::deque<std::vector<char>>& getLastLines();

private:
    class MyVideoWriter : public VideoWriter {
    public:
        MyVideoWriter(EncodeWriter* this_, VideoFormat fmt, int bufsize);
    protected:
        virtual void onWrite(MemoryChunk mc);
    private:
        EncodeWriter* this_;
    };

    MyVideoWriter* videoWriter_;
    StdRedirectedSubProcess* process_;
    bool fieldMode_;
    bool error_;

    // 出力チェック用（なくても処理は問題ない）
    Y4MParser y4mparser;

    void onVideoWrite(MemoryChunk mc);

    VideoFormat getEncoderInputVideoFormat(VideoFormat format);

    // 1つのフレームをトップフィールド、ボトムフィールドの2つのフレームに分解
    static void splitFrameToFields(av::Frame& frame, av::Frame& topfield, av::Frame& bottomfield);
};

} // namespace av


