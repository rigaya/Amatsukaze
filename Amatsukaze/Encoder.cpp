/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "Encoder.h"
#include "EncoderOptionParser.h"
#include <atomic>
#include "rgy_pipe.h"

/* static */ const char* Y4MWriter::getPixelFormat(VideoInfo vi) {
    if (vi.Is420()) {
        switch (vi.BitsPerComponent()) {
        case 8: return "420mpeg2";
        case 10: return "420p10";
        case 12: return "420p12";
        case 14: return "420p14";
        case 16: return "420p16";
        }
    } else if (vi.Is422()) {
        switch (vi.BitsPerComponent()) {
        case 8: return "422";
        case 10: return "422p10";
        case 12: return "422p12";
        case 14: return "422p14";
        case 16: return "422p16";
        }
    } else if (vi.Is444()) {
        switch (vi.BitsPerComponent()) {
        case 8: return "444";
        case 10: return "424p10";
        case 12: return "424p12";
        case 14: return "424p14";
        case 16: return "424p16";
        }
    } else if (vi.IsY()) {
        switch (vi.BitsPerComponent()) {
        case 8: return "mono";
        case 16: return "mono16";
        }
    }
    THROW(FormatException, "サポートされていないフィルタ出力形式です");
    return 0;
}
Y4MWriter::Y4MWriter(VideoInfo vi, VideoFormat outfmt) : n(0) {
    StringBuilder sb;
    sb.append("YUV4MPEG2 W%d H%d C%s I%s F%d:%d A%d:%d",
        outfmt.width, outfmt.height,
        getPixelFormat(vi), outfmt.progressive ? "p" : "t",
        vi.fps_numerator, vi.fps_denominator,
        outfmt.sarWidth, outfmt.sarHeight);
    header = sb.str();
    header.push_back(0x0a);
    frameHeader = "FRAME";
    frameHeader.push_back(0x0a);
    nc = vi.IsY() ? 1 : 3;
}
void Y4MWriter::inputFrame(const PVideoFrame& frame) {
    if (n++ == 0) {
        buffer.add(MemoryChunk((uint8_t*)header.data(), header.size()));
    }
    buffer.add(MemoryChunk((uint8_t*)frameHeader.data(), frameHeader.size()));
    int yuv[] = { PLANAR_Y, PLANAR_U, PLANAR_V };
    for (int c = 0; c < nc; ++c) {
        const uint8_t* plane = frame->GetReadPtr(yuv[c]);
        int pitch = frame->GetPitch(yuv[c]);
        int height = frame->GetHeight(yuv[c]);
        int rowsize = frame->GetRowSize(yuv[c]);
        for (int y = 0; y < height; ++y) {
            buffer.add(MemoryChunk((uint8_t*)plane + y * pitch, rowsize));
        }
        onWrite(buffer.get());
        buffer.clear();
    }
}
/* static */ const char* Y4MEncodeWriter::getYUV(VideoInfo vi) {
    if (vi.Is420()) return "420";
    if (vi.Is422()) return "422";
    if (vi.Is444()) return "424";
    return "Unknown";
}
Y4MEncodeWriter::Y4MEncodeWriter(AMTContext& ctx, const tstring& encoder_args, VideoInfo vi, VideoFormat fmt, bool disablePowerThrottoling)
    : AMTObject(ctx)
    , y4mWriter_(new MyVideoWriter(this, vi, fmt))
    , process_(new StdRedirectedSubProcess(encoder_args, 5, false, disablePowerThrottoling)) {
    ctx.infoF("y4m format: YUV%sp%d %s %dx%d SAR %d:%d %d/%dfps",
        getYUV(vi), vi.BitsPerComponent(), fmt.progressive ? "progressive" : "tff",
        fmt.width, fmt.height, fmt.sarWidth, fmt.sarHeight, vi.fps_numerator, vi.fps_denominator);
}
Y4MEncodeWriter::~Y4MEncodeWriter() {
    if (process_->isRunning()) {
        THROW(InvalidOperationException, "call finish before destroy object ...");
    }
}

void Y4MEncodeWriter::inputFrame(const PVideoFrame& frame) {
    y4mWriter_->inputFrame(frame);
}

void Y4MEncodeWriter::finish() {
    if (y4mWriter_ != NULL) {
        process_->finishWrite();
        int ret = process_->join();
        if (ret != 0) {
            ctx.error("↓↓↓↓↓↓エンコーダ最後の出力↓↓↓↓↓↓");
            for (auto v : process_->getLastLines()) {
                v.push_back(0); // null terminate
                ctx.errorF("%s", v.data());
            }
            ctx.error("↑↑↑↑↑↑エンコーダ最後の出力↑↑↑↑↑↑");
            THROWF(RuntimeException, "エンコーダ終了コード: 0x%x", ret);
        }
    }
}

const std::deque<std::vector<char>>& Y4MEncodeWriter::getLastLines() {
    return process_->getLastLines();
}
Y4MEncodeWriter::MyVideoWriter::MyVideoWriter(Y4MEncodeWriter* this_, VideoInfo vi, VideoFormat fmt)
    : Y4MWriter(vi, fmt)
    , this_(this_) {}
/* virtual */ void Y4MEncodeWriter::MyVideoWriter::onWrite(MemoryChunk mc) {
    this_->onVideoWrite(mc);
}

void Y4MEncodeWriter::onVideoWrite(MemoryChunk mc) {
    process_->write(mc);
}
AMTFilterVideoEncoder::AMTFilterVideoEncoder(
    AMTContext&ctx, int numEncodeBufferFrames)
    : AMTObject(ctx)
    , thread_(this, numEncodeBufferFrames) {
    ctx.infoF("バッファリングフレーム数: %d", numEncodeBufferFrames);
}

void AMTFilterVideoEncoder::encode(
    PClip source, VideoFormat outfmt, const std::vector<double>& timeCodes,
    const std::vector<tstring>& encoderOptions, const int pipeParallel, const bool disablePowerThrottoling,
    IScriptEnvironment* env, const std::function<std::unique_ptr<AMTFilterSource>()>& filterSourceFactory) {
    vi_ = source->GetVideoInfo();
    outfmt_ = outfmt;

    int bufsize = outfmt_.width * outfmt_.height * 3;

    if (timeCodes.size() > 0 && vi_.num_frames != (int)timeCodes.size() - 1) {
        THROW(RuntimeException, "フレーム数が合いません");
    }

    int npass = (int)encoderOptions.size();
    for (int i = 0; i < npass; i++) {
        ctx.infoF("%d/%dパス エンコード開始 予定フレーム数: %d", i + 1, npass, vi_.num_frames);

        const tstring& args = encoderOptions[i];

        // 並列パイプ用の準備 (OS非依存)
        const bool useParallel = (pipeParallel > 0);
        struct ParallelPipeInfo {
            RGYAnonymousPipe pipe;
            int startFrame;
            int endFrame; // [start, end)

            ParallelPipeInfo() : pipe(), startFrame(-1), endFrame(-1) {};
        };
        std::vector<ParallelPipeInfo> pinfo;
        tstring argsWithParallel = args;
        if (useParallel) {
            // フレーム範囲を分割
            const int mp = pipeParallel;
            pinfo.resize(mp);
            for (int p = 0; p < mp; p++) {
                pinfo[p].startFrame = vi_.num_frames * p / mp;
                pinfo[p].endFrame = vi_.num_frames * (p + 1) / mp;
            }

            // 無名パイプ生成 (読み取り側を子プロセスに継承させる)
            StringBuilderT chunkSb;
            chunkSb.append(_T(" --parallel mp=%d,chunk-handles="), mp);
            bool first = true;
            for (int p = 0; p < mp; p++) {
                if (pinfo[p].pipe.create(true, false, 0) != 0) {
                    THROW(RuntimeException, "匿名パイプの生成に失敗");
                }
                if (!first) {
                    chunkSb.append(_T(":"));
                }
                first = false;
                // 子プロセスに継承される読み取りハンドル値を渡す
                chunkSb.append(_T("%llu#%d"), pinfo[p].pipe.childReadableHandleValue(), pinfo[p].startFrame);
            }
            argsWithParallel += chunkSb.str();
        }
        ctx.info("[エンコーダ起動]");
        ctx.infoF("%s", argsWithParallel);

        // 初期化（子プロセス起動）
        encoder_ = std::unique_ptr<Y4MEncodeWriter>(new Y4MEncodeWriter(ctx, argsWithParallel, vi_, outfmt_, disablePowerThrottoling));
        // 親側の読み取りハンドルは不要なので直ちに閉じる（子には継承済み）
        if (useParallel) {
            for (auto& pi : pinfo) {
                pi.pipe.closeRead();
            }
        }

        Stopwatch sw;
        sw.start();

        bool error = false;
        std::atomic<bool> anyError(false);

        try {
            if (useParallel) { // 並列エンコード時
                // Y4Mヘッダのみを標準入力に送るヘルパークラス
                class StdinHeaderWriter : public Y4MWriter {
                public:
                    StdinHeaderWriter(Y4MEncodeWriter* encoder, VideoInfo vi, VideoFormat fmt)
                        : Y4MWriter(vi, fmt), encoder_(encoder) {}
                    void writeHeaderOnly() {
                        // ヘッダーのみを送信（フレームデータは送らない）
                        if (n == 0) {
                            buffer.add(MemoryChunk((uint8_t*)header.data(), header.size()));
                            onWrite(buffer.get());
                            buffer.clear();
                            n++; // ヘッダー送信済みフラグ
                        }
                    }
                protected:
                    virtual void onWrite(MemoryChunk mc) override {
                        encoder_->onVideoWrite(mc);
                    }
                private:
                    Y4MEncodeWriter* encoder_;
                };

                // パイプごとのY4M書き込みスレッド
                class PipeY4MWriter : public Y4MWriter {
                public:
                    PipeY4MWriter(RGYAnonymousPipe* pipe, VideoInfo vi, VideoFormat fmt)
                        : Y4MWriter(vi, fmt), pipe_(pipe) {}
                protected:
                    virtual void onWrite(MemoryChunk mc) override {
                        if (mc.length == 0) return;
                        if (pipe_->write(mc.data, mc.length) != (int)mc.length) {
                            THROW(RuntimeException, "並列パイプへの書き込みに失敗");
                        }
                    }
                private:
                    RGYAnonymousPipe* pipe_;
                };

                class SegmentPumpThread : public DataPumpThread<std::unique_ptr<PVideoFrame>, true> {
                public:
                    SegmentPumpThread(PipeY4MWriter* writer, std::atomic<bool>* anyError)
                        : DataPumpThread(8)
                        , writer_(writer)
                        , anyError_(anyError) {}
                    virtual ~SegmentPumpThread() {
                    }
                protected:
                    virtual void OnDataReceived(std::unique_ptr<PVideoFrame>&& data) override {
                        if (anyError_ && anyError_->load()) {
                            //THROW(RuntimeException, "他スレッドでエラー発生");
                            return;
                        }
                        try {
                            writer_->inputFrame(*data);
                        } catch (Exception&) {
                            if (anyError_) anyError_->store(true);
                            throw; // DataPumpThread 側でerror_に反映される
                        }
                    }
                private:
                    PipeY4MWriter* writer_;
                    std::atomic<bool>* anyError_;
                };

                // 標準入力にY4Mヘッダを送信 (エンコーダの親スレッドが読み取って初期化に使う)
                auto headerWriter = std::unique_ptr<StdinHeaderWriter>(new StdinHeaderWriter(encoder_.get(), vi_, outfmt_));
                headerWriter->writeHeaderOnly();

                // ライタとスレッド生成
                std::vector<std::unique_ptr<PipeY4MWriter>> writers;
                std::vector<std::unique_ptr<SegmentPumpThread>> pumps;
                writers.reserve((int)pinfo.size());
                pumps.reserve((int)pinfo.size());
                for (auto& pi : pinfo) {
                    writers.emplace_back(new PipeY4MWriter(&pi.pipe, vi_, outfmt_));
                    pumps.emplace_back(new SegmentPumpThread(writers.back().get(), &anyError));
                }

                // 各セグメント専用のフィルタチェーンで取得・配送
                // 各スレッドでfilterSourceを構築し、そのGetFrameでstart-endの範囲を取得
                std::vector<std::thread> workers;
                workers.reserve((int)pinfo.size());
                for (int p = 0; p < (int)pinfo.size(); p++) {
                    workers.emplace_back([&](const int threadId) {
                        try {
                            std::unique_ptr<AMTFilterSource> localFilter = filterSourceFactory();
                            IScriptEnvironment2* localenv = localFilter->getEnv();
                            try {
                                // スレッド内で独自のフィルタチェーンを構築
                                PClip localClip = localFilter->getClip(); // fallback
                                pumps[threadId]->start();
                                for (int fi = pinfo[threadId].startFrame; fi < pinfo[threadId].endFrame && !anyError.load(); fi++) {
                                    auto frame = localClip->GetFrame(fi, localenv);
                                    pumps[threadId]->put(std::unique_ptr<PVideoFrame>(new PVideoFrame(frame)), 1);
                                }
                            } catch (const AvisynthError& avserror) {
                                ctx.errorF("Avisynthフィルタでエラーが発生: %s", avserror.msg);
                                anyError.store(true);
                            } catch (Exception&) {
                                anyError.store(true);
                            }
                            pumps[threadId]->join();
                            // localenvがあるうちにデータ(PVideoFrame)をクリアする
                            // そうしないとpumps[threadId]内のデータの破棄時に例外が発生してしまう
                            pumps[threadId]->force_clear();
                        } catch (Exception&) {
                            anyError.store(true);
                        }
                        pinfo[threadId].pipe.closeWrite();
                    }, p);
                }
                for (size_t p = 0; p < workers.size(); p++) {
                    workers[p].join();
                }

                // スレッド終了を待つ
                for (size_t p = 0; p < pumps.size(); p++) {
                    pumps[p]->join();
                }

                // 書き込みハンドルを閉じる (EOF 通知)
                for (auto& pi : pinfo) {
                    pi.pipe.closeWrite();
                }
                workers.clear();
                pumps.clear();
                writers.clear();
                headerWriter.reset();
                error |= anyError.load();
            } else {
                // 既存の単一パイプ処理
                thread_.start();
                for (int i = 0; i < vi_.num_frames; ++i) {
                    auto frame = source->GetFrame(i, env);
                    thread_.put(std::unique_ptr<PVideoFrame>(new PVideoFrame(frame)), 1);
                }
                thread_.join();
            }
        } catch (const AvisynthError& avserror) {
            ctx.errorF("Avisynthフィルタでエラーが発生: %s", avserror.msg);
            error = true;
        } catch (Exception&) {
            error = true;
        } catch (...) {
            error = true;
        }

        // 子プロセスの終了待ち（stdinはfinishで閉じる）。
        // 並列モードでは独自パイプは既に閉じ済み。
        encoder_->finish();

        if (error) {
            THROW(RuntimeException, "エンコード中に不明なエラーが発生");
        }

        encoder_ = nullptr;
        sw.stop();

        // 単一パイプ時のみ従来の待ち時間統計を出す
        if (pipeParallel <= 1) {
            double prod, cons; thread_.getTotalWait(prod, cons);
            ctx.infoF("Total: %.2fs, FilterWait: %.2fs, EncoderWait: %.2fs", sw.getTotal(), prod, cons);
        } else {
            ctx.infoF("Total: %.2fs (parallel mp=%d)", sw.getTotal(), pipeParallel);
        }
    }
}
AMTFilterVideoEncoder::SpDataPumpThread::SpDataPumpThread(AMTFilterVideoEncoder* this_, int bufferingFrames)
    : DataPumpThread(bufferingFrames)
    , this_(this_) {}
/* virtual */ void AMTFilterVideoEncoder::SpDataPumpThread::OnDataReceived(std::unique_ptr<PVideoFrame>&& data) {
    this_->encoder_->inputFrame(*data);
}
AMTSimpleVideoEncoder::AMTSimpleVideoEncoder(
    AMTContext& ctx,
    const ConfigWrapper& setting)
    : AMTObject(ctx)
    , setting_(setting)
    , reader_(this)
    , thread_(this, 8) {
    //
}

void AMTSimpleVideoEncoder::encode() {
    if (setting_.isTwoPass()) {
        ctx.info("1/2パス エンコード開始");
        processAllData(1);
        ctx.info("2/2パス エンコード開始");
        processAllData(2);
    } else {
        processAllData(-1);
    }
}

int AMTSimpleVideoEncoder::getAudioCount() const {
    return audioCount_;
}

int64_t AMTSimpleVideoEncoder::getSrcFileSize() const {
    return srcFileSize_;
}

VideoFormat AMTSimpleVideoEncoder::getVideoFormat() const {
    return videoFormat_;
}
AMTSimpleVideoEncoder::SpVideoReader::SpVideoReader(AMTSimpleVideoEncoder* this_)
    : VideoReader(this_->ctx)
    , this_(this_) {}
/* virtual */ void AMTSimpleVideoEncoder::SpVideoReader::onFileOpen(AVFormatContext *fmt) {
    this_->onFileOpen(fmt);
}
/* virtual */ void AMTSimpleVideoEncoder::SpVideoReader::onVideoFormat(AVStream *stream, VideoFormat fmt) {
    this_->onVideoFormat(stream, fmt);
}
/* virtual */ void AMTSimpleVideoEncoder::SpVideoReader::onFrameDecoded(av::Frame& frame) {
    this_->onFrameDecoded(frame);
}
/* virtual */ void AMTSimpleVideoEncoder::SpVideoReader::onAudioPacket(AVPacket& packet) {
    this_->onAudioPacket(packet);
}
AMTSimpleVideoEncoder::SpDataPumpThread::SpDataPumpThread(AMTSimpleVideoEncoder* this_, int bufferingFrames)
    : DataPumpThread(bufferingFrames)
    , this_(this_) {}
/* virtual */ void AMTSimpleVideoEncoder::SpDataPumpThread::OnDataReceived(std::unique_ptr<av::Frame>&& data) {
    this_->onFrameReceived(std::move(data));
}
AMTSimpleVideoEncoder::AudioFileWriter::AudioFileWriter(AVStream* stream, const tstring& filename, int bufsize)
    : AudioWriter(stream, bufsize)
    , file_(filename, _T("wb")) {}
/* virtual */ void AMTSimpleVideoEncoder::AudioFileWriter::onWrite(MemoryChunk mc) {
    file_.write(mc);
}

void AMTSimpleVideoEncoder::onFileOpen(AVFormatContext *fmt) {
    audioMap_ = std::vector<int>(fmt->nb_streams, -1);
    if (pass_ <= 1) { // 2パス目は出力しない
        audioCount_ = 0;
        for (int i = 0; i < (int)fmt->nb_streams; ++i) {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioFiles_.emplace_back(new AudioFileWriter(
                    fmt->streams[i], setting_.getIntAudioFilePath(EncodeFileKey(), audioCount_, setting_.getAudioEncoder()), 8 * 1024));
                audioMap_[i] = audioCount_++;
            }
        }
    }
}

void AMTSimpleVideoEncoder::processAllData(int pass) {
    pass_ = pass;

    encoder_ = new av::EncodeWriter(ctx);

    // エンコードスレッド開始
    thread_.start();

    // エンコード
    reader_.readAll(setting_.getSrcFilePath(), setting_.getDecoderSetting());

    // エンコードスレッドを終了して自分に引き継ぐ
    thread_.join();

    // 残ったフレームを処理
    encoder_->finish();

    if (pass_ <= 1) { // 2パス目は出力しない
        for (int i = 0; i < audioCount_; ++i) {
            audioFiles_[i]->flush();
        }
        audioFiles_.clear();
    }

    rffExtractor_.clear();
    audioMap_.clear();
    delete encoder_; encoder_ = NULL;
}

void AMTSimpleVideoEncoder::onVideoFormat(AVStream *stream, VideoFormat fmt) {
    videoFormat_ = fmt;

    // ビットレート計算
    File file(setting_.getSrcFilePath(), _T("rb"));
    srcFileSize_ = file.size();
    double srcBitrate = ((double)srcFileSize_ * 8 / 1000) / (stream->duration * av_q2d(stream->time_base));
    ctx.infoF("入力映像ビットレート: %d kbps", (int)srcBitrate);

    if (setting_.isAutoBitrate()) {
        ctx.infoF("目標映像ビットレート: %d kbps",
            (int)setting_.getBitrate().getTargetBitrate(fmt.format, srcBitrate));
    }

    // 初期化
    tstring args = makeEncoderArgs(
        setting_.getEncoder(),
        setting_.getEncoderPath(),
        setting_.getOptions(
            0, fmt.format, srcBitrate, false, pass_, std::vector<BitrateZone>(), tstring(), 1, EncodeFileKey(), EncoderOptionInfo()),
        fmt, tstring(), false,
        setting_.getFormat(),
        setting_.getEncVideoFilePath(EncodeFileKey()));

    ctx.info("[エンコーダ開始]");
    ctx.infoF("%s", args);

    // x265でインタレースの場合はフィールドモード
    bool dstFieldMode =
        (setting_.getEncoder() == ENCODER_X265 && fmt.progressive == false);

    int bufsize = fmt.width * fmt.height * 3;
    encoder_->start(args, fmt, dstFieldMode, bufsize);
}

void AMTSimpleVideoEncoder::onFrameDecoded(av::Frame& frame__) {
    // フレームをコピーしてスレッドに渡す
    thread_.put(std::unique_ptr<av::Frame>(new av::Frame(frame__)), 1);
}

void AMTSimpleVideoEncoder::onFrameReceived(std::unique_ptr<av::Frame>&& frame) {
    // RFFフラグ処理
    // PTSはinputFrameで再定義されるので修正しないでそのまま渡す
    PICTURE_TYPE pic = getPictureTypeFromAVFrame((*frame)());
    //fprintf(stderr, "%s\n", PictureTypeString(pic));
    rffExtractor_.inputFrame(*encoder_, std::move(frame), pic);

    //encoder_.inputFrame(*frame);
}

void AMTSimpleVideoEncoder::onAudioPacket(AVPacket& packet) {
    if (pass_ <= 1) { // 2パス目は出力しない
        int audioIdx = audioMap_[packet.stream_index];
        if (audioIdx >= 0) {
            audioFiles_[audioIdx]->inputFrame(packet);
        }
    }
}
