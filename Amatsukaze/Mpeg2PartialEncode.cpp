#include "Mpeg2PartialEncode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

#include "ReaderWriterFFmpeg.h"
#include "StreamReform.h"
#include "TranscodeSetting.h"
#include "rgy_filesystem.h"

namespace {

constexpr AVRational CLOCK_90K = { 1, MPEG_CLOCK_HZ };
constexpr int64_t TIMESTAMP_MASK = (int64_t{ 1 } << 33) - 1;
constexpr int MIN_COPY_FRAMES = 30;

struct KeepRun {
    int first;
    int last;
};

struct PacketExpectation {
    int64_t pts;
    int64_t dts;
    int size;
    uint64_t hash;
};

struct InputContextDeleter {
    void operator()(AVFormatContext* format) const {
        avformat_close_input(&format);
    }
};

struct OutputContextDeleter {
    void operator()(AVFormatContext* format) const {
        if (format != nullptr) {
            if (format->pb != nullptr) {
                avio_closep(&format->pb);
            }
            avformat_free_context(format);
        }
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const {
        av_packet_free(&packet);
    }
};

tstring avErrorMessage(const char* operation, int error) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return char_to_tstring(std::string(operation) + ": " + buffer);
}

void checkAv(int result, const char* operation) {
    if (result < 0) {
        const auto message = avErrorMessage(operation, result);
        THROWF(FormatException, "%s", message.c_str());
    }
}

uint64_t fnv1a(const uint8_t* data, int size) {
    uint64_t hash = 1469598103934665603ULL;
    for (int i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool timestampEquals(int64_t actual, int64_t expected) {
    if (actual == AV_NOPTS_VALUE || expected == AV_NOPTS_VALUE) {
        return actual == expected;
    }
    return ((actual - expected) & TIMESTAMP_MASK) == 0;
}

bool isAnchor(const VideoFrameInfo& frame) {
    return frame.type == FRAME_I || frame.type == FRAME_P;
}

bool isRestoreI(const VideoFrameInfo& frame) {
    return frame.type == FRAME_I && frame.isGopStart;
}

bool isPatchUnsupportedPicture(PICTURE_TYPE picture) {
    return picture == PIC_FRAME_DOUBLING || picture == PIC_FRAME_TRIPLING
        || picture == PIC_TFF_RFF || picture == PIC_BFF_RFF;
}

std::vector<KeepRun> makeKeepRuns(const EncodeFileInput& file, int numDisplayFrames, tstring& reason) {
    std::vector<KeepRun> runs;
    if (file.videoFrames.empty()) {
        reason = _T("出力対象の映像フレームがありません");
        return runs;
    }
    for (size_t i = 0; i < file.videoFrames.size(); ++i) {
        const int frame = file.videoFrames[i];
        if (frame < 0 || frame >= numDisplayFrames
            || (i > 0 && frame <= file.videoFrames[i - 1])) {
            reason = _T("keep表示順フレーム列が不正です");
            return {};
        }
        if (runs.empty() || frame != runs.back().last) {
            runs.push_back({ frame, frame + 1 });
        } else {
            runs.back().last = frame + 1;
        }
    }
    return runs;
}

void addPatch(std::vector<Mpeg2PartialPatchRange>& patches, int first, int last) {
    if (first < last) {
        patches.push_back({ first, last });
    }
}

void mergePatches(
    std::vector<Mpeg2PartialPatchRange>& patches,
    const std::vector<bool>& keepMask) {
    std::sort(patches.begin(), patches.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    std::vector<Mpeg2PartialPatchRange> merged;
    for (const auto& patch : patches) {
        const bool overlaps = !merged.empty() && patch.first <= merged.back().last;
        const bool hasShortCopyGap = !merged.empty()
            && patch.first >= merged.back().last
            && patch.first - merged.back().last < MIN_COPY_FRAMES
            && std::all_of(keepMask.begin() + merged.back().last,
                keepMask.begin() + patch.first, [](bool keep) { return keep; });
        if (overlaps || hasShortCopyGap) {
            merged.back().last = std::max(merged.back().last, patch.last);
        } else {
            merged.push_back(patch);
        }
    }
    patches = std::move(merged);
}

AVStream* openVideoInput(
    const tstring& path,
    const char* demuxer,
    std::unique_ptr<AVFormatContext, InputContextDeleter>& input) {
    AVFormatContext* rawInput = nullptr;
    const auto* inputFormat = av_find_input_format(demuxer);
    if (inputFormat == nullptr) {
        THROWF(FormatException, "demuxerが見つかりません: %s", char_to_tstring(demuxer).c_str());
    }
    checkAv(avformat_open_input(&rawInput, tchar_to_string(path).c_str(), inputFormat, nullptr),
        "入力映像を開けません");
    input.reset(rawInput);
    checkAv(avformat_find_stream_info(input.get(), nullptr), "ストリーム情報を取得できません");
    const int videoIndex = av_find_best_stream(
        input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    checkAv(videoIndex, "映像ストリームがありません");
    // "mpeg"指定時、FFmpeg 6.1.2のmpegps demuxerは内部で
    // AVSTREAM_PARSE_FULLを設定する。need_parsingは公開AVStreamから外れている。
    return input->streams[videoIndex];
}

void verifyOutput(
    const tstring& outputPath,
    const std::vector<PacketExpectation>& expected) {
    std::unique_ptr<AVFormatContext, InputContextDeleter> input;
    AVStream* stream = openVideoInput(outputPath, "mpegts", input);
    const int videoIndex = stream->index;
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!packet) {
        THROW(RuntimeException, "AVPacketを確保できません");
    }

    size_t index = 0;
    int readResult = 0;
    while ((readResult = av_read_frame(input.get(), packet.get())) >= 0) {
        if (packet->stream_index == videoIndex) {
            if (index >= expected.size()) {
                THROW(FormatException, "出力TSのpicture数が期待値より多いです");
            }
            const auto& item = expected[index];
            const int64_t pts = packet->pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
                : av_rescale_q(packet->pts, stream->time_base, CLOCK_90K);
            const int64_t dts = packet->dts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
                : av_rescale_q(packet->dts, stream->time_base, CLOCK_90K);
            if (!timestampEquals(pts, item.pts) || !timestampEquals(dts, item.dts)) {
                THROWF(FormatException,
                    "出力TSのPTS/DTSが不一致です: packet=%d pts=%lld/%lld dts=%lld/%lld",
                    (int)index, (long long)pts, (long long)item.pts,
                    (long long)dts, (long long)item.dts);
            }
            if (packet->size != item.size || fnv1a(packet->data, packet->size) != item.hash) {
                THROWF(FormatException, "出力TSのCOPY byte列が不一致です: packet=%d", (int)index);
            }
            ++index;
        }
        av_packet_unref(packet.get());
    }
    if (readResult != AVERROR_EOF) {
        checkAv(readResult, "出力TSのpacket読み出しに失敗しました");
    }
    if (index != expected.size()) {
        THROWF(FormatException, "出力TSのpicture数が不一致です: %d/%d",
            (int)index, (int)expected.size());
    }
}

void buildOutput(
    const tstring& inputPath,
    const tstring& outputPath,
    const StreamReformInfo& reformInfo,
    const Mpeg2PartialEncodePlan& plan) {
    std::unique_ptr<AVFormatContext, InputContextDeleter> input;
    AVStream* inputStream = openVideoInput(inputPath, "mpeg", input);
    const int videoIndex = inputStream->index;

    AVFormatContext* rawOutput = nullptr;
    checkAv(avformat_alloc_output_context2(
        &rawOutput, nullptr, "mpegts", tchar_to_string(outputPath).c_str()),
        "MPEG-TS muxerを作成できません");
    std::unique_ptr<AVFormatContext, OutputContextDeleter> output(rawOutput);
    AVStream* outputStream = avformat_new_stream(output.get(), nullptr);
    if (outputStream == nullptr) {
        THROW(RuntimeException, "MPEG-TS映像streamを作成できません");
    }
    checkAv(avcodec_parameters_copy(outputStream->codecpar, inputStream->codecpar),
        "映像codec parameterをコピーできません");
    outputStream->codecpar->codec_tag = 0;
    outputStream->time_base = CLOCK_90K;
    outputStream->avg_frame_rate = inputStream->avg_frame_rate;
    output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    checkAv(avio_open(&output->pb, tchar_to_string(outputPath).c_str(), AVIO_FLAG_WRITE),
        "部分エンコード出力を開けません");
    checkAv(avformat_write_header(output.get(), nullptr), "MPEG-TSヘッダを書けません");

    std::vector<PacketExpectation> expectedOutput;
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!packet) {
        THROW(RuntimeException, "AVPacketを確保できません");
    }

    int filePacketIndex = 0;
    int readResult = 0;
    while ((readResult = av_read_frame(input.get(), packet.get())) >= 0) {
        if (packet->stream_index != videoIndex) {
            av_packet_unref(packet.get());
            continue;
        }
        if (filePacketIndex >= (int)plan.actions.size()) {
            THROW(FormatException, "中間PSのpicture数がFileVideoFrameInfoより多いです");
        }
        const auto& sourceFrame = reformInfo.getVideoFrameInfo(plan.dtsFrameStart + filePacketIndex);
        const int64_t sourceDts = packet->dts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
            : av_rescale_q(packet->dts, inputStream->time_base, CLOCK_90K);
        if (!timestampEquals(sourceDts, sourceFrame.DTS)) {
            THROWF(FormatException,
                "中間PSのDTS mappingが不一致です: packet=%d dts=%lld/%lld",
                filePacketIndex, (long long)sourceDts, (long long)sourceFrame.DTS);
        }
        if (packet->size != sourceFrame.codedDataSize) {
            THROWF(FormatException,
                "中間PSのpacket size mappingが不一致です: packet=%d size=%d/%d",
                filePacketIndex, packet->size, sourceFrame.codedDataSize);
        }

        if (plan.actions[filePacketIndex] == Mpeg2PartialAction::COPY) {
            const auto hash = fnv1a(packet->data, packet->size);
            packet->stream_index = outputStream->index;
            packet->pts = av_rescale_q(plan.pts90k[filePacketIndex], CLOCK_90K, outputStream->time_base);
            packet->dts = av_rescale_q(plan.dts90k[filePacketIndex], CLOCK_90K, outputStream->time_base);
            // DROP前の間隔を持ち越さず、timestampだけを時刻情報の権威とする。
            packet->duration = 0;
            packet->pos = -1;
            expectedOutput.push_back({
                plan.pts90k[filePacketIndex], plan.dts90k[filePacketIndex], packet->size, hash
            });
            checkAv(av_write_frame(output.get(), packet.get()), "MPEG-TS packetを書けません");
        }
        ++filePacketIndex;
        av_packet_unref(packet.get());
    }
    if (readResult != AVERROR_EOF) {
        checkAv(readResult, "中間PSのpacket読み出しに失敗しました");
    }
    if (filePacketIndex != (int)plan.actions.size()) {
        THROWF(FormatException, "中間PSのpicture数が不一致です: %d/%d",
            filePacketIndex, (int)plan.actions.size());
    }
    checkAv(av_write_trailer(output.get()), "MPEG-TS trailerを書けません");
    if (output->pb != nullptr) {
        checkAv(avio_closep(&output->pb), "MPEG-TS出力を閉じられません");
    }
    output.reset();
    input.reset();

    verifyOutput(outputPath, expectedOutput);
}

} // namespace

bool BuildMpeg2PartialEncodePlan(
    const StreamReformInfo& reformInfo,
    EncodeFileKey key,
    Mpeg2PartialEncodePlan& plan,
    tstring& reason) {
    plan = Mpeg2PartialEncodePlan();
    reason.clear();
    const auto& displayFrames = reformInfo.getFilterSourceFrames(key.video);
    const auto& file = reformInfo.getEncodeFile(key);
    const auto frameRange = reformInfo.getVideoFrameRange(key.video);
    const int codedFrameCount = frameRange.second - frameRange.first;
    if (displayFrames.empty() || codedFrameCount <= 0) {
        reason = _T("中間映像ファイルのフレームがありません");
        return false;
    }

    const auto runs = makeKeepRuns(file, (int)displayFrames.size(), reason);
    if (runs.empty()) {
        return false;
    }
    const auto keepSegments = reformInfo.getKeepSegments(key);
    if (keepSegments.size() != runs.size()) {
        reason = _T("keep区間テーブルと表示順フレーム列が一致しません");
        return false;
    }

    std::vector<bool> keepMask(displayFrames.size(), false);
    std::vector<int64_t> outputDisplayPts(displayFrames.size(), AV_NOPTS_VALUE);
    double cumulativeCut = 0;
    for (size_t i = 0; i < runs.size(); ++i) {
        const auto& run = runs[i];
        const auto& segment = keepSegments[i];
        const int64_t expectedStart = (int64_t)std::llround(displayFrames[run.first].pts);
        const int64_t expectedEnd = (int64_t)std::llround(
            displayFrames[run.last - 1].pts + displayFrames[run.last - 1].frameDuration);
        if ((int64_t)std::llround(segment.start) != expectedStart
            || (int64_t)std::llround(segment.end) != expectedEnd) {
            reason = _T("keep区間テーブルのPTSが表示順フレーム列と一致しません");
            return false;
        }
        if (i > 0) {
            cumulativeCut += segment.start - keepSegments[i - 1].end;
        }
        for (int display = run.first; display < run.last; ++display) {
            keepMask[display] = true;
            outputDisplayPts[display] = (int64_t)std::llround(
                displayFrames[display].originalFramePTS - cumulativeCut);
        }
    }

    auto frameAtDisplay = [&](int display) -> const VideoFrameInfo& {
        const int dtsIndex = reformInfo.getDtsFrameIndex(displayFrames[display].frameIndex);
        return reformInfo.getVideoFrameInfo(dtsIndex);
    };

    // 各カット境界の左右で必要になるpatch範囲を表示順で求める。
    for (const auto& run : runs) {
        if (run.first > 0) {
            int restore = run.first;
            while (restore < run.last && !isRestoreI(frameAtDisplay(restore))) {
                ++restore;
            }
            if (restore == run.last && run.last < (int)displayFrames.size()) {
                reason = _T("カット後のkeep区間内にrestoreIがありません");
                return false;
            }
            addPatch(plan.patches, run.first, restore);
        }
        if (run.last < (int)displayFrames.size()) {
            int anchor = run.last - 1;
            while (anchor >= run.first && !isAnchor(frameAtDisplay(anchor))) {
                --anchor;
            }
            if (anchor < run.first) {
                reason = _T("カット前のkeep区間内にlastAnchorがありません");
                return false;
            }
            addPatch(plan.patches, anchor + 1, run.last);
        }
    }
    mergePatches(plan.patches, keepMask);

    for (const auto& patch : plan.patches) {
        const auto baseFormat = frameAtDisplay(patch.first).format;
        for (int display = patch.first; display < patch.last; ++display) {
            const auto& source = frameAtDisplay(display);
            if (displayFrames[display].halfDelay || isPatchUnsupportedPicture(source.pic)) {
                reason = _T("patch範囲にfield pictureまたはRFF付きフレームがあります");
                return false;
            }
            if (source.format != baseFormat) {
                reason = _T("patch範囲に映像フォーマット変化があります");
                return false;
            }
        }
    }

    plan.dtsFrameStart = frameRange.first;
    plan.actions.assign(codedFrameCount, Mpeg2PartialAction::DROP);
    plan.pts90k.assign(codedFrameCount, AV_NOPTS_VALUE);
    plan.dts90k.assign(codedFrameCount, AV_NOPTS_VALUE);
    std::vector<int> firstDisplay(codedFrameCount, -1);
    std::vector<int> keepState(codedFrameCount, -1); // 0: DROP、1: COPY
    std::vector<bool> patchMask(displayFrames.size(), false);
    for (const auto& patch : plan.patches) {
        std::fill(patchMask.begin() + patch.first, patchMask.begin() + patch.last, true);
    }
    std::vector<int> patchState(codedFrameCount, -1); // 0: COPY候補、1: patch
    for (int display = 0; display < (int)displayFrames.size(); ++display) {
        const int globalDts = reformInfo.getDtsFrameIndex(displayFrames[display].frameIndex);
        const int localDts = globalDts - frameRange.first;
        if (localDts < 0 || localDts >= codedFrameCount) {
            reason = _T("表示順からDTS順へのmappingが中間映像ファイル範囲外です");
            return false;
        }
        const int currentState = keepMask[display] ? 1 : 0;
        if (keepState[localDts] >= 0 && keepState[localDts] != currentState) {
            reason = _T("RFFの同一符号化pictureがkeepとdropの境界を跨いでいます");
            return false;
        }
        keepState[localDts] = currentState;
        if (firstDisplay[localDts] < 0) {
            firstDisplay[localDts] = display;
        }
        if (keepMask[display]) {
            const int currentPatchState = patchMask[display] ? 1 : 0;
            if (patchState[localDts] >= 0 && patchState[localDts] != currentPatchState) {
                reason = _T("RFFの同一符号化pictureがpatchとCOPYの境界を跨いでいます");
                return false;
            }
            patchState[localDts] = currentPatchState;
        }
    }

    // patchに置き換えるpictureはCOPY対象から外す。フェーズ2では後段でフォールバックする。
    for (int localDts = 0; localDts < codedFrameCount; ++localDts) {
        if (patchState[localDts] == 1) {
            keepState[localDts] = 0;
        }
    }

    std::vector<int64_t> displayOrderPicturePts;
    for (int localDts = 0; localDts < codedFrameCount; ++localDts) {
        if (keepState[localDts] == 1) {
            const int display = firstDisplay[localDts];
            if (display < 0 || outputDisplayPts[display] == AV_NOPTS_VALUE) {
                reason = _T("COPY pictureの表示順PTSを取得できません");
                return false;
            }
            plan.actions[localDts] = Mpeg2PartialAction::COPY;
            plan.pts90k[localDts] = outputDisplayPts[display];
            displayOrderPicturePts.push_back(outputDisplayPts[display]);
        }
    }
    std::sort(displayOrderPicturePts.begin(), displayOrderPicturePts.end());
    if (displayOrderPicturePts.empty()) {
        reason = _T("COPY pictureがありません");
        return false;
    }
    if (std::adjacent_find(displayOrderPicturePts.begin(), displayOrderPicturePts.end())
        != displayOrderPicturePts.end()) {
        reason = _T("出力pictureの表示順PTSが重複しています");
        return false;
    }

    const auto& format = reformInfo.getFormat(key).videoFormat;
    const int64_t frameDuration = (int64_t)std::llround(
        format.frameRateDenom * (double)MPEG_CLOCK_HZ / format.frameRateNum);
    size_t outputIndex = 0;
    int64_t previousDts = std::numeric_limits<int64_t>::min();
    const int64_t firstDts = displayOrderPicturePts.front() - frameDuration;
    if (firstDts < 0) {
        reason = _T("出力DTSが負になるためフル再エンコードへフォールバックします");
        return false;
    }
    for (int localDts = 0; localDts < codedFrameCount; ++localDts) {
        if (plan.actions[localDts] != Mpeg2PartialAction::COPY) {
            continue;
        }
        // フェーズ3ではpatch pictureのPTSもdisplayOrderPicturePtsへ加えてから同じ式を使う。
        const int64_t dts = outputIndex == 0
            ? displayOrderPicturePts.front() - frameDuration
            : displayOrderPicturePts[outputIndex - 1];
        plan.dts90k[localDts] = dts;
        if (dts <= previousDts || dts > plan.pts90k[localDts]) {
            reason = _T("出力DTSの単調性またはDTS <= PTS条件を満たしません");
            return false;
        }
        previousDts = dts;
        ++outputIndex;
    }
    if (outputIndex != displayOrderPicturePts.size()) {
        reason = _T("符号化順と表示順の出力picture数が一致しません");
        return false;
    }
    for (int display = 0; display < (int)displayFrames.size(); ++display) {
        const int globalDts = reformInfo.getDtsFrameIndex(displayFrames[display].frameIndex);
        const bool copied = plan.actions[globalDts - frameRange.first] == Mpeg2PartialAction::COPY;
        if (keepMask[display] != (copied || patchMask[display])
            || (copied && patchMask[display])) {
            reason = _T("keep表示位置とCOPY/patchプランが1:1対応していません");
            return false;
        }
    }
    return true;
}

bool TryMpeg2PartialEncode(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    EncodeFileKey key,
    tstring& reason) {
    reason.clear();
    const tstring outputPath = setting.getEncVideoFilePath(key);
    try {
        Mpeg2PartialEncodePlan plan;
        if (!BuildMpeg2PartialEncodePlan(reformInfo, key, plan, reason)) {
            return false;
        }
        if (!plan.patches.empty()) {
            reason = StringFormat(
                _T("patchが%d区間必要ですが、patchエンコードはフェーズ3で実装します"),
                (int)plan.patches.size());
            return false;
        }

        buildOutput(setting.getIntVideoFilePath(key.video), outputPath, reformInfo, plan);
        ctx.infoF(_T("[MPEG-2部分エンコード] COPYのみで%d pictureを出力しました"),
            (int)std::count(plan.actions.begin(), plan.actions.end(), Mpeg2PartialAction::COPY));
        return true;
    } catch (const Exception& e) {
        reason = e.message();
    } catch (const std::exception& e) {
        reason = char_to_tstring(e.what());
    }
    if (File::exists(outputPath)) {
        rgy_file_remove(outputPath.c_str());
    }
    return false;
}
