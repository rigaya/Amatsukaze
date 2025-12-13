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
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "rgy_pipe.h"
#include "StringUtils.h"
#include <cmath>
#include "TranscodeManager.h"
#include "rgy_filesystem.h"

namespace {

bool isNativeParallelEncoder(ENUM_ENCODER encoder) {
    return encoder == ENCODER_QSVENC || encoder == ENCODER_NVENC || encoder == ENCODER_VCEENC;
}

bool isSoftwareSplitEncoder(ENUM_ENCODER encoder) {
    return encoder == ENCODER_X264 || encoder == ENCODER_X265 || encoder == ENCODER_SVTAV1;
}

// 実際の動画尺(秒)を取得する
// - VFR の場合は timeCodes (ms、要素数=フレーム数+1) を優先する
// - timeCodes が無い/不正な場合は fps から算出する (CFR向け)
double calcDurationSec(const VideoInfo& vi, const std::vector<double>& timeCodes) {
    if (!timeCodes.empty() && (int)timeCodes.size() == vi.num_frames + 1) {
        const double startMs = timeCodes.front();
        const double endMs = timeCodes.back();
        const double durationMs = endMs - startMs;
        if (durationMs > 0.0 && std::isfinite(durationMs)) {
            return durationMs / 1000.0;
        }
    }
    if (vi.fps_numerator > 0) {
        return vi.num_frames * vi.fps_denominator / (double)vi.fps_numerator;
    }
    return 0.0;
}

tstring appendChunkSuffix(const tstring& path, int chunkIndex) {
    return strsprintf(_T("%s.chunk%d%s"), PathRemoveExtensionS(path).c_str(), chunkIndex, rgy_get_extension(path).c_str());
}

std::vector<BitrateZone> sliceBitrateZones(const std::vector<BitrateZone>& zones, int chunkStart, int chunkEnd) {
    std::vector<BitrateZone> result;
    for (const auto& zone : zones) {
        const int interStart = std::max(zone.startFrame, chunkStart);
        const int interEnd = std::min(zone.endFrame, chunkEnd);
        if (interStart >= interEnd) {
            continue;
        }
        BitrateZone chunkZone = zone;
        chunkZone.startFrame = interStart - chunkStart;
        chunkZone.endFrame = interEnd - chunkStart;
        result.push_back(chunkZone);
    }
    return result;
}

tstring createChunkTimecodeFile(const tstring& basePath, int chunkIndex, int startFrame, int endFrame, const std::vector<double>& timeCodes, AMTContext& ctx) {
    if (basePath.size() == 0 || timeCodes.size() == 0 || endFrame <= startFrame) {
        return _T("");
    }
    tstring chunkPath = appendChunkSuffix(basePath, chunkIndex);
    ctx.registerTmpFile(chunkPath);
    File file(chunkPath, _T("wb"));
    const char header[] = "# timecode format v2\n";
    file.write(MemoryChunk((uint8_t*)header, sizeof(header) - 1));
    const double base = timeCodes[startFrame];
    for (int frame = startFrame; frame < endFrame; frame++) {
        const int64_t value = (int64_t)std::llround(timeCodes[frame] - base);
        std::string line = std::to_string((long long)value);
        line.push_back('\n');
        file.write(MemoryChunk((uint8_t*)line.data(), (int)line.size()));
    }
    return chunkPath;
}

void concatenateChunkOutputs(const tstring& finalPath, const std::vector<tstring>& chunkPaths) {
    if (chunkPaths.empty()) {
        return;
    }
    std::vector<uint8_t> buffer(4 * 1024 * 1024);
    File finalFile(finalPath, _T("wb"));
    for (const auto& chunk : chunkPaths) {
        File chunkFile(chunk, _T("rb"));
        while (true) {
            MemoryChunk mc(buffer.data(), (int)buffer.size());
            size_t bytes = chunkFile.read(mc);
            if (bytes == 0) {
                break;
            }
            finalFile.write(MemoryChunk(buffer.data(), (int)bytes));
        }
    }
}

} // anonymous namespace

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
    for (int c = 0; c < nc; c++) {
        const uint8_t* plane = frame->GetReadPtr(yuv[c]);
        int pitch = frame->GetPitch(yuv[c]);
        int height = frame->GetHeight(yuv[c]);
        int rowsize = frame->GetRowSize(yuv[c]);
        for (int y = 0; y < height; y++) {
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
Y4MEncodeWriter::Y4MEncodeWriter(AMTContext& ctx, const tstring& encoder_args, VideoInfo vi, VideoFormat fmt, bool disablePowerThrottoling, bool captureOutputOnly, StdRedirectedSubProcess::LineCallback lineCallback)
    : AMTObject(ctx)
    , y4mWriter_(new MyVideoWriter(this, vi, fmt))
    , process_(new StdRedirectedSubProcess(encoder_args, 5, false, disablePowerThrottoling, captureOutputOnly, lineCallback)) {
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

const std::vector<std::vector<char>>& Y4MEncodeWriter::getCapturedLines() const {
    return process_->getCapturedLines();
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
    AMTContext&ctx, const ConfigWrapper& setting, int numEncodeBufferFrames)
    : AMTObject(ctx)
    , vi_()
    , outfmt_()
    , setting_(setting)
    , encoder_()
    , pipeParallel_(0)
    , thread_(this, numEncodeBufferFrames) {
    ctx.infoF("バッファリングフレーム数: %d", numEncodeBufferFrames);
}

void AMTFilterVideoEncoder::encodeSWParallel(
    EncoderArgumentGenerator& argGen,
    const std::vector<double>& timeCodes,
    const std::vector<BitrateZone>& bitrateZones,
    double vfrBitrateScale,
    const tstring& timecodePath,
    int vfrTimingFps,
    const tstring& baseOutputPath,
    EncodeFileKey key,
    int serviceId,
    const EncoderOptionInfo& eoInfo,
    int currentPass,
    int passIndex,
    int actualParallel,
    bool disablePowerThrottoling,
    const std::function<std::unique_ptr<AMTFilterSource>()>& filterSourceFactory) {
    if (!filterSourceFactory) {
        THROW(RuntimeException, "分割エンコードにはfilterSourceFactoryが必要です");
    }
    const int mp = actualParallel;
    struct ChunkTask {
        int startFrame = 0;
        int endFrame = 0;
        tstring args;
        tstring outputPath;
    };
    std::vector<ChunkTask> chunks(mp);
    std::vector<tstring> chunkOutputs;
    chunkOutputs.reserve(mp);

    for (int p = 0; p < mp; p++) {
        auto& chunk = chunks[p];
        chunk.startFrame = vi_.num_frames * p / mp;
        chunk.endFrame = vi_.num_frames * (p + 1) / mp;
        const int chunkFrames = chunk.endFrame - chunk.startFrame;
        auto chunkZones = sliceBitrateZones(bitrateZones, chunk.startFrame, chunk.endFrame);
        tstring chunkTimecodePath;
        if (timecodePath.size() > 0) {
            chunkTimecodePath = createChunkTimecodeFile(timecodePath, passIndex * mp + p, chunk.startFrame, chunk.endFrame, timeCodes, ctx);
        }
        chunk.outputPath = appendChunkSuffix(baseOutputPath, passIndex * mp + p);
        ctx.registerTmpFile(chunk.outputPath);
        chunk.args = argGen.GenEncoderOptions(
            chunkFrames,
            outfmt_, std::move(chunkZones), vfrBitrateScale,
            chunkTimecodePath, vfrTimingFps, key, currentPass, serviceId, eoInfo, chunk.outputPath);
        chunkOutputs.push_back(chunk.outputPath);
    }

    class ChunkLogManager {
    public:
        ChunkLogManager(AMTContext& ctx, int count)
            : ctx_(ctx)
            , entries_(count)
            , stop_(false)
            , finishedCount_(0) {}

        StdRedirectedSubProcess::LineCallback makeCallback(int idx) {
            return [this, idx](bool isErr, const std::vector<char>& line, bool isProgress) {
                std::lock_guard<std::mutex> lock(entries_[idx].mtx);
                if (isProgress) {
                    entries_[idx].lastProgress.assign(line.begin(), line.end());
                    entries_[idx].hasProgress = true;
                } else {
                    entries_[idx].logs.emplace_back(line.begin(), line.end());
                }
                progressCv_.notify_all();
            };
        }

        void start() {
            progressThread_ = std::thread([this]() { progressLoop(); });
        }

        void markFinished(int idx) {
            {
                std::lock_guard<std::mutex> lock(entries_[idx].mtx);
                entries_[idx].finished = true;
            }
            finishedCount_.fetch_add(1);
            progressCv_.notify_all();
        }

        void stop() {
            {
                std::lock_guard<std::mutex> lock(progressMutex_);
                stop_ = true;
            }
            progressCv_.notify_all();
            if (progressThread_.joinable()) {
                progressThread_.join();
            }
        }

        void dumpLogs() {
            for (size_t idx = 0; idx < entries_.size(); ++idx) {
                std::lock_guard<std::mutex> lock(entries_[idx].mtx);
                for (const auto& line : entries_[idx].logs) {
                    ctx_.infoF("[chunk%d] %s", (int)idx, line.c_str());
                }
            }
        }

    private:
        struct Entry {
            std::mutex mtx;
            std::vector<std::string> logs;
            std::string lastProgress;
            bool hasProgress = false;
            bool finished = false;
        };

        bool allFinished() const {
            return finishedCount_.load() == (int)entries_.size();
        }

        void progressLoop() {
            size_t offset = 0;
            while (true) {
                {
                    std::lock_guard<std::mutex> lock(progressMutex_);
                    if (stop_) break;
                }
                bool printed = false;
                for (size_t attempt = 0; attempt < entries_.size(); ++attempt) {
                    size_t idx = (offset + attempt) % entries_.size();
                    std::unique_lock<std::mutex> lock(entries_[idx].mtx);
                    if (entries_[idx].finished) {
                        continue;
                    }
                    std::string message = entries_[idx].hasProgress ? entries_[idx].lastProgress : std::string("Running...");
                    lock.unlock();
                    ctx_.progressF("[chunk%d] %s", (int)idx, message.c_str());
                    offset = idx + 1;
                    printed = true;
                    break;
                }
                if (allFinished()) {
                    break;
                }
                std::unique_lock<std::mutex> lock(progressMutex_);
                progressCv_.wait_for(lock, std::chrono::seconds(1), [this]() { return stop_.load(); });
                if (stop_.load()) {
                    break;
                }
                if (!printed && entries_.empty()) {
                    break;
                }
            }
        }

        AMTContext& ctx_;
        std::vector<Entry> entries_;
        std::mutex progressMutex_;
        std::condition_variable progressCv_;
        std::atomic<bool> stop_;
        std::atomic<int> finishedCount_;
        std::thread progressThread_;
    };

    ChunkLogManager logManager(ctx, mp);
    logManager.start();
    bool logStopped = false;
    auto stopLogs = [&]() {
        if (!logStopped) {
            logManager.stop();
            logManager.dumpLogs();
            logStopped = true;
        }
    };

    ctx.info("[エンコーダ起動]");
    for (int p = 0; p < mp; p++) {
        ctx.infoF("[chunk %d] %s", p, chunks[p].args.c_str());
    }

    class ChunkPumpThread : public DataPumpThread<std::unique_ptr<PVideoFrame>, true> {
    public:
        ChunkPumpThread(Y4MEncodeWriter* encoder, std::atomic<bool>* anyError)
            : DataPumpThread(8)
            , encoder_(encoder)
            , anyError_(anyError) {}
    protected:
        virtual void OnDataReceived(std::unique_ptr<PVideoFrame>&& data) override {
            if (anyError_ && anyError_->load()) {
                return;
            }
            try {
                encoder_->inputFrame(*data);
            } catch (Exception&) {
                if (anyError_) anyError_->store(true);
                throw;
            }
        }
    private:
        Y4MEncodeWriter* encoder_;
        std::atomic<bool>* anyError_;
    };

    Stopwatch sw;
    sw.start();

    bool error = false;
    std::atomic<bool> anyError(false);
    std::vector<std::unique_ptr<Y4MEncodeWriter>> chunkEncoders(mp);
    std::vector<std::unique_ptr<ChunkPumpThread>> chunkPumps;
    chunkPumps.reserve(mp);
    std::vector<std::thread> workers;
    workers.reserve(mp);
    std::vector<std::shared_ptr<AMTFilterSource>> chunkFilters(mp);
    double totalEncodeTime = 0.0;

    try {
        try {
            for (int p = 0; p < mp; p++) {
                chunkEncoders[p] = std::unique_ptr<Y4MEncodeWriter>(new Y4MEncodeWriter(
                    ctx, chunks[p].args, vi_, outfmt_, disablePowerThrottoling, true, logManager.makeCallback(p)));
                chunkPumps.emplace_back(new ChunkPumpThread(chunkEncoders[p].get(), &anyError));
                chunkPumps.back()->start();
            }

            for (int p = 0; p < mp; p++) {
                workers.emplace_back([&, p]() {
                    try {
                        std::shared_ptr<AMTFilterSource> localFilter(filterSourceFactory().release());
                        chunkFilters[p] = localFilter;
                        IScriptEnvironment2* localenv = localFilter->getEnv();
                        PClip localClip = localFilter->getClip();
                        for (int fi = chunks[p].startFrame; fi < chunks[p].endFrame && !anyError.load(); ++fi) {
                            auto frame = localClip->GetFrame(fi, localenv);
                            chunkPumps[p]->put(std::unique_ptr<PVideoFrame>(new PVideoFrame(frame)), 1);
                        }
                    } catch (const AvisynthError& avserror) {
                        ctx.errorF("Avisynthフィルタでエラーが発生: %s", avserror.msg);
                        anyError.store(true);
                    } catch (Exception&) {
                        anyError.store(true);
                    }
                    chunkPumps[p]->join();
                    chunkPumps[p]->force_clear();
                    chunkFilters[p].reset();
                });
            }

            for (auto& worker : workers) {
                worker.join();
            }
        } catch (const AvisynthError& avserror) {
            ctx.errorF("Avisynthフィルタでエラーが発生: %s", avserror.msg);
            error = true;
        } catch (Exception&) {
            error = true;
        } catch (...) {
            error = true;
        }

        if (anyError.load()) {
            error = true;
        }

        for (int p = 0; p < mp; p++) {
            if (chunkEncoders[p]) {
                chunkEncoders[p]->finish();
            }
            logManager.markFinished(p);
        }
        stopLogs();

        if (error) {
            THROW(RuntimeException, "エンコード中に不明なエラーが発生");
        }

        // エンコード全体の経過時間を計測
        sw.stop();
        totalEncodeTime = sw.getTotal();
        ctx.infoF("%d並列エンコード完了 %.2fs", mp, totalEncodeTime);

        if (setting_.getEncoder() == ENCODER_SVTAV1) {
            // SVT-AV1 はバイナリ連結できないため、mp4boxを使用してチャンクを結合する
            const tstring mp4boxPath = setting_.getMp4BoxPath();
            const tstring tmpDir = setting_.getTmpDir();

            if (mp4boxPath.size() == 0) {
                THROW(RuntimeException, "SVT-AV1の分割エンコード結合に必要なmp4boxのパスが設定されていません");
            }

            auto runMp4BoxWithLogging = [&](const tstring& cmdLine) {
                ctx.infoF("MP4Box コマンド: %s", cmdLine.c_str());
                StdRedirectedSubProcess proc(cmdLine, 0, true, false, true);
                int ret = proc.join();
                const auto& lines = proc.getCapturedLines();
                if (!lines.empty()) {
                    ctx.info("MP4Box 出力↓↓↓↓↓↓");
                    for (auto v : lines) {
                        auto line = v;
                        line.push_back('\0');
                        ctx.infoF("%s", line.data());
                    }
                    ctx.info("MP4Box 出力↑↑↑↑↑↑");
                }
                // mp4boxがコンソール出力のコードページを変えてしまうので戻す
                ctx.setDefaultCP();
                if (ret != 0) {
                    THROWF(RuntimeException, "MP4Box結合処理がエラーコード(%d)を返しました", ret);
                }
            };

            // まず各チャンクの生AV1出力を個別のMP4に変換
            std::vector<tstring> chunkMp4List;
            chunkMp4List.reserve(chunkOutputs.size());
            for (const auto& chunkPath : chunkOutputs) {
                tstring chunkMp4 = chunkPath + _T(".mp4");
                ctx.registerTmpFile(chunkMp4);

                StringBuilderT sb;
                sb.append(_T("\"%s\""), mp4boxPath.c_str());
                sb.append(_T(" -brand mp42 -ab mp41 -ab iso2"));
                sb.append(_T(" -tmp \"%s\""), tmpDir.c_str());
                sb.append(_T(" -add \"%s#video:name=Video:forcesync"), chunkPath.c_str());
                if (outfmt_.fixedFrameRate) {
                    sb.append(_T(":fps=%d/%d"), outfmt_.frameRateNum, outfmt_.frameRateDenom);
                }
                sb.append(_T("\""));
                sb.append(_T(" -new \"%s\""), chunkMp4.c_str());

                runMp4BoxWithLogging(sb.str());
                chunkMp4List.push_back(chunkMp4);
            }

            // 生成したMP4をmp4boxで結合して最終出力(baseOutputPath)とする
            if (!chunkMp4List.empty()) {
                StringBuilderT sb;
                sb.append(_T("\"%s\""), mp4boxPath.c_str());
                sb.append(_T(" -tmp \"%s\""), tmpDir.c_str());
                // 1つ目を-add、以降を-catで連結
                sb.append(_T(" -add \"%s#video:name=Video:forcesync\""), chunkMp4List[0].c_str());
                for (size_t i = 1; i < chunkMp4List.size(); ++i) {
                    sb.append(_T(" -cat \"%s\""), chunkMp4List[i].c_str());
                }
                sb.append(_T(" -new \"%s\""), baseOutputPath.c_str());

                runMp4BoxWithLogging(sb.str());
            }
        } else {
            // x264/x265など従来通りバイナリ連結
            concatenateChunkOutputs(baseOutputPath, chunkOutputs);
        }
    } catch (...) {
        stopLogs();
        throw;
    }
    chunkFilters.clear();
    // 実効fps, 実効ビットレートを計算して表示
    const double effectiveFps = (totalEncodeTime > 0.0)
        ? (vi_.num_frames / totalEncodeTime)
        : 0.0;
    // 実効bitrateはbaseOutputPathのファイルサイズから算出する
    // 分母はduration
    const double duration = calcDurationSec(vi_, timeCodes);
    uint64_t fileSize = 0;
    if (!rgy_get_filesize(baseOutputPath.c_str(), &fileSize)) {
        ctx.infoF("%d並列エンコード 実効速度: %.2f fps", mp, effectiveFps);
    } else if (fileSize == 0) {
        THROW(RuntimeException, "出力映像ファイルサイズが0です");
    } else if (duration <= 0.0) {
        ctx.infoF("%d並列エンコード 実効速度: %.2f fps, 実効ビットレート: (duration不明)", mp, effectiveFps);
    } else {
        const double effectiveBitrate = fileSize * 8 / (duration * 1000.0);
        ctx.infoF("%d並列エンコード 実効速度: %.2f fps, 実効ビットレート: %.2f kbps", mp, effectiveFps, effectiveBitrate);
    }
}

void AMTFilterVideoEncoder::encode(
    PClip source, VideoFormat outfmt, const std::vector<double>& timeCodes,
    EncoderArgumentGenerator& argGen, const std::vector<int>& passList,
    const std::vector<BitrateZone>& bitrateZones, double vfrBitrateScale,
    const tstring& timecodePath, int vfrTimingFps, const tstring& baseOutputPath,
    EncodeFileKey key, int serviceId, const EncoderOptionInfo& eoInfo,
    const int pipeParallel, const bool disablePowerThrottoling,
    IScriptEnvironment* env, const std::function<std::unique_ptr<AMTFilterSource>()>& filterSourceFactory,
    ENUM_ENCODER encoderType) {
    vi_ = source->GetVideoInfo();
    outfmt_ = outfmt;

    int bufsize = outfmt_.width * outfmt_.height * 3;

    if (timeCodes.size() > 0 && vi_.num_frames != (int)timeCodes.size() - 1) {
        THROW(RuntimeException, "フレーム数が合いません");
    }

    const bool wantsParallel = pipeParallel > 1;
    const bool nativeParallel = wantsParallel && isNativeParallelEncoder(encoderType);
    const bool softwareParallel = wantsParallel && !nativeParallel && isSoftwareSplitEncoder(encoderType);
    const int actualParallel = (nativeParallel || softwareParallel) ? pipeParallel : 1;

    const int npass = (int)passList.size();
    for (int i = 0; i < npass; i++) {
        const int currentPass = passList[i];
        ctx.infoF("%d/%dパス エンコード開始 予定フレーム数: %d", i + 1, npass, vi_.num_frames);

        if (softwareParallel) {
            if (npass > 1) {
                THROW(RuntimeException, "分割エンコードは2passと同時に使用できません");
            }
            encodeSWParallel(
                argGen, timeCodes, bitrateZones, vfrBitrateScale,
                timecodePath, vfrTimingFps, baseOutputPath,
                key, serviceId, eoInfo, currentPass, i,
                actualParallel, disablePowerThrottoling, filterSourceFactory);
            continue;
        }

        tstring args = argGen.GenEncoderOptions(
            vi_.num_frames,
            outfmt_, bitrateZones, vfrBitrateScale,
            timecodePath, vfrTimingFps, key, currentPass, serviceId, eoInfo, baseOutputPath);

        // 並列パイプ用の準備 (OS非依存)
        const bool useParallel = nativeParallel;
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
            const int mp = actualParallel;
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
                for (int i = 0; i < vi_.num_frames; i++) {
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
        if (actualParallel <= 1) {
            double prod, cons; thread_.getTotalWait(prod, cons);
            ctx.infoF("Total: %.2fs, FilterWait: %.2fs, EncoderWait: %.2fs", sw.getTotal(), prod, cons);
        } else {
            ctx.infoF("Total: %.2fs (parallel mp=%d)", sw.getTotal(), actualParallel);
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
        for (int i = 0; i < (int)fmt->nb_streams; i++) {
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
        for (int i = 0; i < audioCount_; i++) {
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
