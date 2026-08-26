/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "Encoder.h"
#include "EncoderOptionParser.h"
#include <algorithm>
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

bool isSoftwareSplitEncoder(ENUM_ENCODER encoder) {
    return encoder == ENCODER_X264 || encoder == ENCODER_X265 || encoder == ENCODER_SVTAV1;
}

namespace {

bool isNativeParallelEncoder(ENUM_ENCODER encoder) {
    return encoder == ENCODER_QSVENC || encoder == ENCODER_NVENC || encoder == ENCODER_VCEENC;
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

struct FrameChunk {
    int startFrame;
    int endFrame;
    bool isCM = false;
};

std::vector<FrameChunk> createEqualFrameChunks(int numFrames, int chunkCount) {
    std::vector<FrameChunk> chunks;
    chunks.reserve(chunkCount);
    for (int i = 0; i < chunkCount; i++) {
        chunks.push_back({
            numFrames * i / chunkCount,
            numFrames * (i + 1) / chunkCount
        });
    }
    return chunks;
}

std::vector<FrameChunk> createCMAwareFrameChunks(
    int numFrames,
    int chunkCount,
    const std::vector<EncoderZone>& cmzones) {
    if (cmzones.empty() || numFrames <= 0) {
        return createEqualFrameChunks(numFrames, chunkCount);
    }

    std::vector<int> boundaries = { 0, numFrames };
    boundaries.reserve(2 + cmzones.size() * 2);
    for (const auto& zone : cmzones) {
        if (0 < zone.startFrame && zone.startFrame < numFrames) {
            boundaries.push_back(zone.startFrame);
        }
        if (0 < zone.endFrame && zone.endFrame < numFrames) {
            boundaries.push_back(zone.endFrame);
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    const double idealChunkLength = (double)numFrames / std::max(chunkCount, 1);
    std::vector<FrameChunk> chunks;
    for (size_t intervalIndex = 0; intervalIndex + 1 < boundaries.size(); intervalIndex++) {
        const int intervalStart = boundaries[intervalIndex];
        const int intervalEnd = boundaries[intervalIndex + 1];
        const int intervalLength = intervalEnd - intervalStart;
        // 境界はcmzonesの端点から作っているので、区間の先頭がCM区間に入っていれば区間全体がCM
        const bool isCM = std::any_of(cmzones.begin(), cmzones.end(), [&](const EncoderZone& zone) {
            return zone.startFrame <= intervalStart && intervalStart < zone.endFrame;
        });
        const int splitCount = std::max(1, (int)std::llround(intervalLength / idealChunkLength));
        for (int i = 0; i < splitCount; i++) {
            chunks.push_back({
                intervalStart + intervalLength * i / splitCount,
                intervalStart + intervalLength * (i + 1) / splitCount,
                isCM
            });
        }
    }
    return chunks;
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
        // フレーム番号がチャンク内相対になるため、絶対時刻の表示時刻は無効化する
        // (分割エンコードはソフトウェアエンコーダ専用で、時刻指定ゾーンは使用しない)
        chunkZone.startSec = BITRATE_ZONE_SEC_UNSET;
        chunkZone.endSec = BITRATE_ZONE_SEC_UNSET;
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

std::vector<double> readEncoderFilterTimecode(const tstring& path) {
    static const int64_t MAX_TIMECODE_FILE_SIZE = 16 * 1024 * 1024;
    std::string text;
    {
        File input(path, _T("rb"));
        const int64_t fileSize = input.size();
        if (fileSize <= 0 || fileSize > MAX_TIMECODE_FILE_SIZE) {
            THROW(FormatException, "エンコーダフィルタのチャンクタイムコードが空または大きすぎます");
        }
        text.resize((size_t)fileSize);
        if (input.read(MemoryChunk((uint8_t*)text.data(), (size_t)fileSize)) != (size_t)fileSize) {
            THROW(RuntimeException, "エンコーダフィルタのチャンクタイムコードを読み込めません");
        }
    }

    std::vector<double> timestamps;
    size_t begin = 0;
    int lineNumber = 0;
    while (begin < text.size()) {
        const size_t end = text.find('\n', begin);
        const std::string line = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        begin = (end == std::string::npos) ? text.size() : end + 1;
        lineNumber++;
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        try {
            size_t parsed = 0;
            const double timestamp = std::stod(line.substr(first), &parsed);
            if (!std::isfinite(timestamp)
                || line.find_first_not_of(" \t\r", first + parsed) != std::string::npos) {
                THROW(FormatException, "エンコーダフィルタのチャンクタイムコード形式が不正です");
            }
            timestamps.push_back(timestamp);
        } catch (const Exception&) {
            throw;
        } catch (...) {
            THROWF(FormatException, "エンコーダフィルタのチャンクタイムコード形式が不正です (%d行目)", lineNumber);
        }
    }
    if (timestamps.empty()) {
        THROW(FormatException, "エンコーダフィルタがタイムコードを出力しませんでした。"
            "raw出力時の--timecode出力に対応したエンコーダが必要です。"
            "エンコーダフィルタに指定したエンコーダを最新版に更新してください");
    }
    return timestamps;
}

void concatenateEncoderFilterTimecodes(
    const tstring& finalPath,
    const std::vector<tstring>& chunkPaths,
    const std::vector<int>& startFrames,
    const VideoInfo& vi) {
    if (chunkPaths.size() != startFrames.size() || vi.fps_numerator <= 0) {
        THROW(RuntimeException, "エンコーダフィルタのチャンクタイムコードを連結できません");
    }

    // チャンク境界の時刻は入力フレーム番号から求めるため、フレームレートが変化していると
    // 境界のフレーム間隔だけが前後とずれる。これをCFR/VFR判定から除外できるよう、
    // 境界にあたる出力フレーム番号をコメント行として記録しておく
    std::vector<std::string> lines;
    std::vector<int> boundaries;
    for (size_t p = 0; p < chunkPaths.size(); p++) {
        const auto timestamps = readEncoderFilterTimecode(chunkPaths[p]);
        if (p > 0) {
            boundaries.push_back((int)lines.size());
        }
        const double base = timestamps.front();
        const double chunkStart = startFrames[p] * 1000.0 * vi.fps_denominator / vi.fps_numerator;
        for (const auto timestamp : timestamps) {
            lines.push_back(std::to_string(timestamp - base + chunkStart));
            lines.back().push_back('\n');
        }
    }

    File output(finalPath, _T("wb"));
    const char header[] = "# timecode format v2\n";
    output.write(MemoryChunk((uint8_t*)header, sizeof(header) - 1));
    if (boundaries.size() > 0) {
        std::string comment = "# " + std::string(ENCODER_FILTER_CHUNK_BOUNDARY_TAG);
        for (size_t i = 0; i < boundaries.size(); i++) {
            comment += (i == 0 ? " " : ",") + std::to_string(boundaries[i]);
        }
        comment.push_back('\n');
        output.write(MemoryChunk((uint8_t*)comment.data(), comment.size()));
    }
    for (const auto& line : lines) {
        output.write(MemoryChunk((uint8_t*)line.data(), line.size()));
    }
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
Y4MWriter::Y4MWriter(VideoInfo vi, VideoFormat outfmt, bool sarInContainerOnly) : n(0) {
    // sarInContainerOnly 時はエンコーダにSAR比を渡さない（コンテナ側で記録するため1:1にする）
    const int y4mSarW = sarInContainerOnly ? 1 : outfmt.sarWidth;
    const int y4mSarH = sarInContainerOnly ? 1 : outfmt.sarHeight;
    StringBuilder sb;
    sb.append("YUV4MPEG2 W%d H%d C%s I%s F%d:%d A%d:%d",
        outfmt.width, outfmt.height,
        getPixelFormat(vi), outfmt.progressive ? "p" : "t",
        vi.fps_numerator, vi.fps_denominator,
        y4mSarW, y4mSarH);
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
Y4MEncodeWriter::Y4MEncodeWriter(AMTContext& ctx, const tstring& encoder_args, VideoInfo vi, VideoFormat fmt, bool disablePowerThrottoling, bool captureOutputOnly, StdRedirectedSubProcess::LineCallback lineCallback, bool sarInContainerOnly, const tstring& filterArgs)
    : AMTObject(ctx)
    , y4mWriter_(new MyVideoWriter(this, vi, fmt, sarInContainerOnly))
    , filterProcess_()
    , process_()
    , brokenY4MDelimiterCount_(0) {
    PIPE_HANDLE externalStdIn = (PIPE_HANDLE)0;
    if (!filterArgs.empty()) {
        ctx.info(_T("[エンコーダフィルタ起動]"));
        ctx.infoF(_T("%s"), filterArgs);
        // 引数のctxはコンストラクタを抜けると寿命が尽きるため、メンバのctxを使う
        auto filterLineCallback = [this](bool, const std::vector<char>& line, bool) {
            this->ctx.infoF(_T("[エンコーダフィルタ] %s"), char_to_tstring(std::string(line.begin(), line.end())));
        };
        filterProcess_.reset(new StdRedirectedSubProcess(filterArgs, 5, false, disablePowerThrottoling, true, filterLineCallback, (PIPE_HANDLE)0, true));
        // フィルタのstdout読み取り端は所有権ごと受け取り、本エンコーダのstdinに渡す
        externalStdIn = filterProcess_->detachStdOutReadHandle();
    }
    try {
        // SVT-AV1はY4Mのフレーム境界が壊れていても終了コード0を返すことがあるため、
        // 固有のエラーメッセージも監視して、finish()で必ず失敗として扱う。
        auto encoderLineCallback = [this, lineCallback](bool isErr, const std::vector<char>& line, bool isProgress) {
            const std::string message(line.begin(), line.end());
            if (message.find("Failed to read proper y4m frame delimiter") != std::string::npos) {
                brokenY4MDelimiterCount_.fetch_add(1, std::memory_order_relaxed);
            }
            if (lineCallback) {
                lineCallback(isErr, line, isProgress);
            }
        };
        process_.reset(new StdRedirectedSubProcess(encoder_args, 5, false, disablePowerThrottoling, captureOutputOnly, encoderLineCallback, externalStdIn));
    } catch (...) {
        if (filterProcess_) {
            // 本エンコーダに渡せなかった読み取り端を閉じる
            closePipeHandle(externalStdIn);
            // stdinを閉じないとフィルタが入力待ちのままjoin()が返らない
            filterProcess_->finishWrite();
            filterProcess_->join();
        }
        throw;
    }
    const int logSarW = sarInContainerOnly ? 1 : fmt.sarWidth;
    const int logSarH = sarInContainerOnly ? 1 : fmt.sarHeight;
    ctx.infoF(_T("y4m format: YUV%sp%d %s %dx%d SAR %d:%d %d/%dfps"),
        char_to_tstring(getYUV(vi)), vi.BitsPerComponent(), fmt.progressive ? _T("progressive") : _T("tff"),
        fmt.width, fmt.height, logSarW, logSarH, vi.fps_numerator, vi.fps_denominator);
}
Y4MEncodeWriter::~Y4MEncodeWriter() {
    if ((filterProcess_ && filterProcess_->isRunning()) || process_->isRunning()) {
        THROW(InvalidOperationException, "call finish before destroy object ...");
    }
}

void Y4MEncodeWriter::inputFrame(const PVideoFrame& frame) {
    y4mWriter_->inputFrame(frame);
}

void Y4MEncodeWriter::finish() {
    if (y4mWriter_ != NULL) {
        int filterRet = 0;
        if (filterProcess_) {
            filterProcess_->finishWrite();
            filterRet = filterProcess_->join();
        } else {
            process_->finishWrite();
        }
        const int ret = process_->join();
        if (filterRet != 0) {
            ctx.error(_T("↓↓↓↓↓↓エンコーダフィルタ最後の出力↓↓↓↓↓↓"));
            for (auto v : filterProcess_->getLastLines()) {
                v.push_back(0); // null terminate
                ctx.errorF(_T("[エンコーダフィルタ] %s"), char_to_tstring(v.data()));
            }
            ctx.error(_T("↑↑↑↑↑↑エンコーダフィルタ最後の出力↑↑↑↑↑↑"));
        }
        if (ret != 0) {
            ctx.error(_T("↓↓↓↓↓↓エンコーダ最後の出力↓↓↓↓↓↓"));
            for (auto v : process_->getLastLines()) {
                v.push_back(0); // null terminate
                ctx.errorF(_T("%s"), char_to_tstring(v.data()));
            }
            ctx.error(_T("↑↑↑↑↑↑エンコーダ最後の出力↑↑↑↑↑↑"));
        }
        if (filterRet != 0) {
            THROWF(RuntimeException, "エンコーダフィルタ終了コード: 0x%x", filterRet);
        }
        if (ret != 0) {
            THROWF(RuntimeException, "エンコーダ終了コード: 0x%x", ret);
        }
        const int brokenDelimiterCount = brokenY4MDelimiterCount_.load(std::memory_order_relaxed);
        if (brokenDelimiterCount > 0) {
            THROWF(RuntimeException, "Y4Mフレーム境界の破損を検出しました: %d回", brokenDelimiterCount);
        }
    }
}

void Y4MEncodeWriter::closeInput() {
    if (y4mWriter_ != NULL) {
        (filterProcess_ ? filterProcess_.get() : process_.get())->finishWrite();
    }
}

const std::deque<std::vector<char>>& Y4MEncodeWriter::getLastLines() {
    return process_->getLastLines();
}

const std::vector<std::vector<char>>& Y4MEncodeWriter::getCapturedLines() const {
    return process_->getCapturedLines();
}
Y4MEncodeWriter::MyVideoWriter::MyVideoWriter(Y4MEncodeWriter* this_, VideoInfo vi, VideoFormat fmt, bool sarInContainerOnly)
    : Y4MWriter(vi, fmt, sarInContainerOnly)
    , this_(this_) {}
/* virtual */ void Y4MEncodeWriter::MyVideoWriter::onWrite(MemoryChunk mc) {
    this_->onVideoWrite(mc);
}

void Y4MEncodeWriter::onVideoWrite(MemoryChunk mc) {
    (filterProcess_ ? filterProcess_.get() : process_.get())->write(mc);
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
    ctx.infoF(_T("バッファリングフレーム数: %d"), numEncodeBufferFrames);
}

void AMTFilterVideoEncoder::encodeSWParallel(
    EncoderArgumentGenerator& argGen,
    const std::vector<double>& timeCodes,
    const std::vector<BitrateZone>& bitrateZones,
    const std::vector<EncoderZone>& cmzones,
    bool useCMChunkSplit,
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
    const auto chunkRanges = useCMChunkSplit
        ? createCMAwareFrameChunks(vi_.num_frames, mp, cmzones)
        : createEqualFrameChunks(vi_.num_frames, mp);
    const int numChunks = (int)chunkRanges.size();
    struct ChunkTask {
        int startFrame = 0;
        int endFrame = 0;
        tstring args;
        tstring filterArgs;
        tstring filterTimecodePath;
        tstring outputPath;
        bool isCM = false;
    };
    std::vector<ChunkTask> chunks(numChunks);
    std::vector<tstring> chunkOutputs;
    chunkOutputs.reserve(numChunks);

    VideoFormat encoderInputFormat = outfmt_;
    if (setting_.isEncoderFilterSeparate() && setting_.isEncoderFilterDeinterlace()) {
        encoderInputFormat.progressive = true;
    }
    // フィルタによるフレームレート・フレーム数の変化はオプション文字列の解析では正確に
    // 判定できないため、常にタイムコードを出力させ、エンコード後に実測値から判定する
    const tstring encoderFilterTimecodePath = setting_.isEncoderFilterSeparate()
        ? setting_.getEncoderFilterTimecodePath(key)
        : tstring();
    const bool outputFilterTimecode = !encoderFilterTimecodePath.empty();
    std::vector<tstring> chunkFilterTimecodePaths;
    std::vector<int> chunkStartFrames;
    if (outputFilterTimecode) {
        chunkFilterTimecodePaths.reserve(numChunks);
        chunkStartFrames.reserve(numChunks);
    }

    for (int i = 0; i < numChunks; i++) {
        auto& chunk = chunks[i];
        chunk.startFrame = chunkRanges[i].startFrame;
        chunk.endFrame = chunkRanges[i].endFrame;
        chunk.isCM = chunkRanges[i].isCM;
        const int chunkFrames = chunk.endFrame - chunk.startFrame;
        auto chunkZones = useCMChunkSplit
            ? std::vector<BitrateZone>()
            : sliceBitrateZones(bitrateZones, chunk.startFrame, chunk.endFrame);
        tstring chunkTimecodePath;
        if (timecodePath.size() > 0) {
            chunkTimecodePath = createChunkTimecodeFile(timecodePath, passIndex * numChunks + i, chunk.startFrame, chunk.endFrame, timeCodes, ctx);
        }
        chunk.outputPath = appendChunkSuffix(baseOutputPath, passIndex * numChunks + i);
        ctx.registerTmpFile(chunk.outputPath);
        if (outputFilterTimecode) {
            chunk.filterTimecodePath = appendChunkSuffix(encoderFilterTimecodePath, passIndex * numChunks + i);
            ctx.registerTmpFile(chunk.filterTimecodePath);
            chunkFilterTimecodePaths.push_back(chunk.filterTimecodePath);
            chunkStartFrames.push_back(chunk.startFrame);
        }
        // 別プロセスのフィルタは間引き等で出力フレーム数を変更し得る。
        // 入力フレーム数を本エンコーダの上限にすると、正常なEOFを破損入力として扱う
        // エンコーダがあるため、パイプのEOFで終了させる。
        const int encoderFrameCount = setting_.isEncoderFilterSeparate() ? 0 : chunkFrames;
        chunk.args = argGen.GenEncoderOptions(
            encoderFrameCount,
            encoderInputFormat, std::move(chunkZones), vfrBitrateScale,
            chunkTimecodePath, vfrTimingFps, key, currentPass, serviceId, eoInfo, chunk.outputPath, chunk.isCM);
        chunk.filterArgs = setting_.isEncoderFilterSeparate()
            ? makeEncoderFilterArgs(setting_.getEncoderFilterPath(), setting_.getEncoderFilterOptions(), outfmt_, chunk.filterTimecodePath,
                setting_.getEncoder())
            : tstring();
        chunkOutputs.push_back(chunk.outputPath);
    }

    class ChunkLogManager {
    public:
        ChunkLogManager(AMTContext& ctx, int count)
            : ctx_(ctx)
            , entries_(count)
            , stop_(false)
            , finishedCount_(0) {}

        void beginChunk(int slot, int chunkIndex) {
            std::lock_guard<std::mutex> lock(entries_[slot].mtx);
            entries_[slot].chunkIndex = chunkIndex;
            entries_[slot].lastProgress.clear();
            entries_[slot].hasProgress = false;
            progressCv_.notify_all();
        }

        StdRedirectedSubProcess::LineCallback makeCallback(int slot) {
            return [this, slot](bool isErr, const std::vector<char>& line, bool isProgress) {
                std::lock_guard<std::mutex> lock(entries_[slot].mtx);
                if (isProgress) {
                    entries_[slot].lastProgress.assign(line.begin(), line.end());
                    entries_[slot].hasProgress = true;
                } else {
                    entries_[slot].logs.push_back({
                        entries_[slot].chunkIndex,
                        std::string(line.begin(), line.end())
                    });
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
            for (size_t slot = 0; slot < entries_.size(); slot++) {
                std::lock_guard<std::mutex> lock(entries_[slot].mtx);
                for (const auto& line : entries_[slot].logs) {
                    ctx_.infoF(_T("[slot%d] chunk%d %s"), (int)slot, line.chunkIndex, char_to_tstring(line.text));
                }
            }
        }

    private:
        struct LogLine {
            int chunkIndex;
            std::string text;
        };

        struct Entry {
            std::mutex mtx;
            std::vector<LogLine> logs;
            std::string lastProgress;
            int chunkIndex = -1;
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
                for (size_t attempt = 0; attempt < entries_.size(); attempt++) {
                    size_t slot = (offset + attempt) % entries_.size();
                    std::unique_lock<std::mutex> lock(entries_[slot].mtx);
                    if (entries_[slot].finished || entries_[slot].chunkIndex < 0) {
                        continue;
                    }
                    const int chunkIndex = entries_[slot].chunkIndex;
                    std::string message = entries_[slot].hasProgress ? entries_[slot].lastProgress : std::string("Running...");
                    lock.unlock();
                    ctx_.progressF(_T("[slot%d] chunk%d %s"), (int)slot, chunkIndex, char_to_tstring(message));
                    offset = slot + 1;
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

    ctx.info(_T("[エンコーダ起動]"));
    for (int i = 0; i < numChunks; i++) {
        ctx.infoF(_T("[chunk %d] %s"), i, chunks[i].args.c_str());
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
    std::vector<int> executionOrder;
    executionOrder.reserve(numChunks);
    for (int i = 0; i < numChunks; i++) {
        executionOrder.push_back(i);
    }
    std::stable_sort(executionOrder.begin(), executionOrder.end(), [&](int lhs, int rhs) {
        return chunks[lhs].endFrame - chunks[lhs].startFrame
            > chunks[rhs].endFrame - chunks[rhs].startFrame;
    });
    std::atomic<int> nextTask(0);
    std::vector<std::thread> workers;
    workers.reserve(mp);
    double totalEncodeTime = 0.0;

    try {
        try {
            for (int slot = 0; slot < mp; slot++) {
                workers.emplace_back([&, slot]() {
                    try {
                        std::unique_ptr<AMTFilterSource> localFilter = filterSourceFactory();
                        IScriptEnvironment2* localenv = localFilter->getEnv();
                        PClip localClip = localFilter->getClip();
                        while (!anyError.load()) {
                            const int orderIndex = nextTask.fetch_add(1);
                            if (orderIndex >= numChunks || anyError.load()) {
                                break;
                            }
                            const int chunkIndex = executionOrder[orderIndex];
                            const auto& chunk = chunks[chunkIndex];
                            logManager.beginChunk(slot, chunkIndex);

                            std::unique_ptr<Y4MEncodeWriter> encoder;
                            std::unique_ptr<ChunkPumpThread> pump;
                            bool finishStarted = false;
                            try {
                                encoder.reset(new Y4MEncodeWriter(
                                    ctx, chunk.args, vi_, outfmt_, disablePowerThrottoling, true,
                                    logManager.makeCallback(slot), setting_.getSARInContainerOnly(), chunk.filterArgs));
                                pump.reset(new ChunkPumpThread(encoder.get(), &anyError));
                                pump->start();
                                for (int fi = chunk.startFrame; fi < chunk.endFrame && !anyError.load(); fi++) {
                                    auto frame = localClip->GetFrame(fi, localenv);
                                    pump->put(std::unique_ptr<PVideoFrame>(new PVideoFrame(frame)), 1);
                                }
                                pump->join();
                                pump->force_clear();

                                // フィルタプロセスのハンドル継承を同一ワーカー内で完結させてから次へ進む。
                                encoder->closeInput();
                                finishStarted = true;
                                encoder->finish();
                            } catch (...) {
                                anyError.store(true);
                                if (pump && pump->isRunning()) {
                                    pump->join();
                                }
                                if (pump) {
                                    pump->force_clear();
                                }
                                if (encoder && !finishStarted) {
                                    try {
                                        encoder->closeInput();
                                        encoder->finish();
                                    } catch (Exception&) {
                                    }
                                }
                                throw;
                            }
                        }
                    } catch (const AvisynthError& avserror) {
                        ctx.errorF(_T("Avisynthフィルタでエラーが発生: %s"), char_to_tstring(avserror.msg));
                        anyError.store(true);
                    } catch (Exception&) {
                        anyError.store(true);
                    } catch (...) {
                        anyError.store(true);
                    }
                    logManager.markFinished(slot);
                });
            }

        } catch (const AvisynthError& avserror) {
            ctx.errorF(_T("Avisynthフィルタでエラーが発生: %s"), char_to_tstring(avserror.msg));
            anyError.store(true);
            error = true;
        } catch (Exception&) {
            anyError.store(true);
            error = true;
        } catch (...) {
            anyError.store(true);
            error = true;
        }

        for (auto& worker : workers) {
            worker.join();
        }

        if (anyError.load()) {
            error = true;
        }
        stopLogs();

        if (error) {
            THROW(RuntimeException, "エンコード中に不明なエラーが発生");
        }

        if (outputFilterTimecode) {
            concatenateEncoderFilterTimecodes(encoderFilterTimecodePath, chunkFilterTimecodePaths, chunkStartFrames, vi_);
        }

        // エンコード全体の経過時間を計測
        sw.stop();
        totalEncodeTime = sw.getTotal();
        ctx.infoF(_T("%d並列エンコード完了 %.2fs"), mp, totalEncodeTime);

        if (setting_.getEncoder() == ENCODER_SVTAV1) {
            // SVT-AV1 はバイナリ連結できないため、mp4boxを使用してチャンクを結合する
            const tstring mp4boxPath = setting_.getMp4BoxPath();
            const tstring tmpDir = setting_.getTmpDir();

            if (mp4boxPath.size() == 0) {
                THROW(RuntimeException, "SVT-AV1の分割エンコード結合に必要なmp4boxのパスが設定されていません");
            }

            auto runMp4BoxWithLogging = [&](const tstring& cmdLine) {
                ctx.infoF(_T("MP4Box コマンド: %s"), cmdLine.c_str());
                StdRedirectedSubProcess proc(cmdLine, 0, true, false, true);
                int ret = proc.join();
                const auto& lines = proc.getCapturedLines();
                if (!lines.empty()) {
                    ctx.info(_T("MP4Box 出力↓↓↓↓↓↓"));
                    for (auto v : lines) {
                        auto line = v;
                        line.push_back('\0');
                        ctx.infoF(_T("%s"), char_to_tstring(line.data()));
                    }
                    ctx.info(_T("MP4Box 出力↑↑↑↑↑↑"));
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
                for (size_t i = 1; i < chunkMp4List.size(); i++) {
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
    // 実効fps, 実効ビットレートを計算して表示
    const double effectiveFps = (totalEncodeTime > 0.0)
        ? (vi_.num_frames / totalEncodeTime)
        : 0.0;
    // 実効bitrateはbaseOutputPathのファイルサイズから算出する
    // 分母はduration
    const double duration = calcDurationSec(vi_, timeCodes);
    uint64_t fileSize = 0;
    if (!rgy_get_filesize(baseOutputPath.c_str(), &fileSize)) {
        ctx.infoF(_T("%d並列エンコード 実効速度: %.2f fps"), mp, effectiveFps);
    } else if (fileSize == 0) {
        THROW(RuntimeException, "出力映像ファイルサイズが0です");
    } else if (duration <= 0.0) {
        ctx.infoF(_T("%d並列エンコード 実効速度: %.2f fps, 実効ビットレート: (duration不明)"), mp, effectiveFps);
    } else {
        const double effectiveBitrate = fileSize * 8 / (duration * 1000.0);
        ctx.infoF(_T("%d並列エンコード 実効速度: %.2f fps, 実効ビットレート: %.2f kbps"), mp, effectiveFps, effectiveBitrate);
    }
}

void AMTFilterVideoEncoder::encode(
    PClip source, VideoFormat outfmt, const std::vector<double>& timeCodes,
    EncoderArgumentGenerator& argGen, const std::vector<int>& passList,
    const std::vector<BitrateZone>& bitrateZones,
    const std::vector<EncoderZone>& cmzones, bool useCMChunkSplit,
    double vfrBitrateScale,
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
    const bool softwareParallel = (wantsParallel || useCMChunkSplit) && !nativeParallel && isSoftwareSplitEncoder(encoderType);
    const int actualParallel = (nativeParallel || softwareParallel) ? pipeParallel : 1;
    // フィルタによるフレームレート・フレーム数の変化はオプション文字列の解析では正確に
    // 判定できないため、常にタイムコードを出力させ、エンコード後に実測値から判定する
    const tstring encoderFilterTimecodePath = setting_.isEncoderFilterSeparate()
        ? setting_.getEncoderFilterTimecodePath(key)
        : tstring();

    const int npass = (int)passList.size();
    for (int i = 0; i < npass; i++) {
        const int currentPass = passList[i];
        ctx.infoF(_T("%d/%dパス エンコード開始 予定フレーム数: %d"), i + 1, npass, vi_.num_frames);

        if (softwareParallel) {
            if (npass > 1) {
                THROW(RuntimeException, "分割エンコードは2passと同時に使用できません");
            }
            encodeSWParallel(
                argGen, timeCodes, bitrateZones, cmzones, useCMChunkSplit, vfrBitrateScale,
                timecodePath, vfrTimingFps, baseOutputPath,
                key, serviceId, eoInfo, currentPass, i,
                actualParallel, disablePowerThrottoling, filterSourceFactory);
            continue;
        }

        VideoFormat encoderInputFormat = outfmt_;
        if (setting_.isEncoderFilterSeparate() && setting_.isEncoderFilterDeinterlace()) {
            // 前段でインタレース解除済みのため、本エンコーダにはprogressiveとして渡す
            encoderInputFormat.progressive = true;
        }
        // エンコーダフィルタでフレーム数が変わる場合は、入力側のフレーム数を
        // 本エンコーダへ指定せず、フィルタ出力パイプのEOFで終了させる。
        const int encoderFrameCount = setting_.isEncoderFilterSeparate() ? 0 : vi_.num_frames;
        tstring args = argGen.GenEncoderOptions(
            encoderFrameCount,
            encoderInputFormat, bitrateZones, vfrBitrateScale,
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
        ctx.info(_T("[エンコーダ起動]"));
        ctx.infoF(_T("%s"), argsWithParallel);

        // 初期化（子プロセス起動）
        const tstring filterArgs = setting_.isEncoderFilterSeparate()
            ? makeEncoderFilterArgs(setting_.getEncoderFilterPath(), setting_.getEncoderFilterOptions(), outfmt_, encoderFilterTimecodePath,
                setting_.getEncoder())
            : tstring();
        encoder_ = std::unique_ptr<Y4MEncodeWriter>(new Y4MEncodeWriter(ctx, argsWithParallel, vi_, outfmt_, disablePowerThrottoling, false, StdRedirectedSubProcess::LineCallback(), setting_.getSARInContainerOnly(), filterArgs));
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
                    StdinHeaderWriter(Y4MEncodeWriter* encoder, VideoInfo vi, VideoFormat fmt, bool sarInContainerOnly)
                        : Y4MWriter(vi, fmt, sarInContainerOnly), encoder_(encoder) {}
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
                    PipeY4MWriter(RGYAnonymousPipe* pipe, VideoInfo vi, VideoFormat fmt, bool sarInContainerOnly)
                        : Y4MWriter(vi, fmt, sarInContainerOnly), pipe_(pipe) {}
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
                auto headerWriter = std::unique_ptr<StdinHeaderWriter>(new StdinHeaderWriter(encoder_.get(), vi_, outfmt_, setting_.getSARInContainerOnly()));
                headerWriter->writeHeaderOnly();

                // ライタとスレッド生成
                std::vector<std::unique_ptr<PipeY4MWriter>> writers;
                std::vector<std::unique_ptr<SegmentPumpThread>> pumps;
                writers.reserve((int)pinfo.size());
                pumps.reserve((int)pinfo.size());
                for (auto& pi : pinfo) {
                    writers.emplace_back(new PipeY4MWriter(&pi.pipe, vi_, outfmt_, setting_.getSARInContainerOnly()));
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
                                ctx.errorF(_T("Avisynthフィルタでエラーが発生: %s"), char_to_tstring(avserror.msg));
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
            ctx.errorF(_T("Avisynthフィルタでエラーが発生: %s"), char_to_tstring(avserror.msg));
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
            ctx.infoF(_T("Total: %.2fs, FilterWait: %.2fs, EncoderWait: %.2fs"), sw.getTotal(), prod, cons);
        } else {
            ctx.infoF(_T("Total: %.2fs (parallel mp=%d)"), sw.getTotal(), actualParallel);
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
        ctx.info(_T("1/2パス エンコード開始"));
        processAllData(1);
        ctx.info(_T("2/2パス エンコード開始"));
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
    ctx.infoF(_T("入力映像ビットレート: %d kbps"), (int)srcBitrate);

    if (setting_.isAutoBitrate()) {
        ctx.infoF(_T("目標映像ビットレート: %d kbps"),
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

    ctx.info(_T("[エンコーダ開始]"));
    ctx.infoF(_T("%s"), args);

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
