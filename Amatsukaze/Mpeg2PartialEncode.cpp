#include "Mpeg2PartialEncode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

#include "AMTSource.h"
#include "Encoder.h"
#include "Mpeg2VideoParser.h"
#include "OSUtil.h"
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

struct PatchEncodedData {
    std::vector<std::vector<uint8_t>> pictures;
};

struct VbvSetting {
    bool available = false;
    int maxrateKbps = 0;
    int bufferKbit = 0;
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

VbvSetting readOriginalVbv(
    const tstring& inputPath,
    const StreamReformInfo& reformInfo,
    const Mpeg2PartialEncodePlan& plan) {
    const auto copy = std::find_if(plan.outputEntries.begin(), plan.outputEntries.end(),
        [](const auto& entry) { return entry.kind == Mpeg2PartialAction::COPY; });
    if (copy == plan.outputEntries.end()) {
        return {};
    }

    std::unique_ptr<AVFormatContext, InputContextDeleter> input;
    AVStream* stream = openVideoInput(inputPath, "mpeg", input);
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!packet) {
        THROW(RuntimeException, "AVPacketを確保できません");
    }
    int localDts = 0;
    int readResult = 0;
    while ((readResult = av_read_frame(input.get(), packet.get())) >= 0) {
        if (packet->stream_index != stream->index) {
            av_packet_unref(packet.get());
            continue;
        }
        if (localDts == copy->localDts) {
            const auto& sourceFrame = reformInfo.getVideoFrameInfo(plan.dtsFrameStart + localDts);
            if (packet->size != sourceFrame.codedDataSize) {
                THROW(FormatException, "VBV取得時の中間PS mappingが不一致です");
            }
            for (int i = 0; i + 4 <= packet->size; ++i) {
                if (packet->data[i] == 0x00 && packet->data[i + 1] == 0x00
                    && packet->data[i + 2] == 0x01 && packet->data[i + 3] == 0xB3) {
                    MPEG2SequenceHeader header = {};
                    if (!header.parse(packet->data + i, packet->size - i)) {
                        return {};
                    }
                    const uint64_t bitrateValue =
                        ((uint64_t)header.bit_rate_extension << 18) | header.bit_rate_value;
                    const uint64_t vbvValue =
                        ((uint64_t)header.vbv_buffer_size_extension << 10)
                        | header.vbv_buffer_size_value;
                    return {
                        true,
                        (int)((bitrateValue * 400 + 999) / 1000),
                        (int)((vbvValue * 16 * 1024 + 999) / 1000)
                    };
                }
            }
            return {};
        }
        ++localDts;
        av_packet_unref(packet.get());
    }
    if (readResult != AVERROR_EOF) {
        checkAv(readResult, "VBV取得用packetの読み出しに失敗しました");
    }
    return {};
}

void removeSequenceEndCode(const tstring& path) {
    std::vector<uint8_t> data;
    {
        File input(path, _T("rb"));
        const int64_t fileSize = input.size();
        if (fileSize < 4) {
            return;
        }
        if (fileSize > std::numeric_limits<int>::max()) {
            THROW(FormatException, "patch ESが大きすぎます");
        }
        data.resize((size_t)fileSize);
        if (input.read(MemoryChunk(data.data(), (int)data.size())) != data.size()) {
            THROW(IOException, "patch ESを読み出せません");
        }
    }
    const size_t end = data.size();
    if (data[end - 4] != 0x00 || data[end - 3] != 0x00
        || data[end - 2] != 0x01 || data[end - 1] != 0xB7) {
        return;
    }
    data.resize(end - 4);
    File output(path, _T("wb"));
    output.write(MemoryChunk(data.data(), (int)data.size()));
}

PatchEncodedData readPatchPictures(const tstring& path, int expectedPictures) {
    std::unique_ptr<AVFormatContext, InputContextDeleter> input;
    AVStream* stream = openVideoInput(path, "mpegvideo", input);
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!packet) {
        THROW(RuntimeException, "AVPacketを確保できません");
    }
    PatchEncodedData result;
    int readResult = 0;
    while ((readResult = av_read_frame(input.get(), packet.get())) >= 0) {
        if (packet->stream_index == stream->index) {
            result.pictures.emplace_back(packet->data, packet->data + packet->size);
        }
        av_packet_unref(packet.get());
    }
    if (readResult != AVERROR_EOF) {
        checkAv(readResult, "patch ESのpacket読み出しに失敗しました");
    }
    if ((int)result.pictures.size() != expectedPictures) {
        THROWF(FormatException, "patch ESのpicture数が不一致です: %d/%d",
            (int)result.pictures.size(), expectedPictures);
    }
    return result;
}

tstring makePatchEncoderArgs(
    const ConfigWrapper& setting,
    const VideoFormat& format,
    const tstring& outputPath,
    int frames,
    bool bottomFieldFirst,
    const VbvSetting& vbv) {
    StringBuilderT sb;
    sb.append(_T("\"%s\" --mpeg2 --demuxer y4m"), setting.getEncoderPath())
        .append(_T(" --keyint infinite --bframes 0 --min-keyint 1 --scenecut 0"));
    if (!format.progressive) {
        sb.append(bottomFieldFirst ? _T(" --bff") : _T(" --tff"));
    }
    int darWidth = 0;
    int darHeight = 0;
    format.getDAR(darWidth, darHeight);
    // x262はMPEG-2時だけ--sarをsequence headerのDARコードとして扱う。
    sb.append(_T(" --fps %d/%d --sar %d:%d"),
        format.frameRateNum, format.frameRateDenom, darWidth, darHeight);
    if (format.colorPrimaries != AVCOL_PRI_UNSPECIFIED) {
        sb.append(_T(" --colorprim %s"),
            char_to_tstring(av::getColorPrimStr(format.colorPrimaries, ENCODER_X262)));
    }
    if (format.transferCharacteristics != AVCOL_TRC_UNSPECIFIED) {
        sb.append(_T(" --transfer %s"),
            char_to_tstring(av::getTransferCharacteristicsStr(
                format.transferCharacteristics, ENCODER_X262)));
    }
    if (format.colorSpace != AVCOL_SPC_UNSPECIFIED) {
        sb.append(_T(" --colormatrix %s"),
            char_to_tstring(av::getColorSpaceStr(format.colorSpace, ENCODER_X262)));
    }
    sb.append(_T(" --crf 18"));
    if (vbv.available) {
        sb.append(_T(" --vbv-maxrate %d --vbv-bufsize %d"),
            vbv.maxrateKbps, vbv.bufferKbit);
    }
    sb.append(_T(" --frames %d -o \"%s\" -"), frames, outputPath);
    return sb.str();
}

std::vector<PatchEncodedData> encodePatches(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    EncodeFileKey key,
    const Mpeg2PartialEncodePlan& plan,
    const tstring& outputPath) {
    std::vector<PatchEncodedData> result;
    result.reserve(plan.patches.size());
    if (plan.patches.empty()) {
        return result;
    }
    const auto& displayFrames = reformInfo.getFilterSourceFrames(key.video);
    const auto& format = reformInfo.getFormat(key).videoFormat;
    const auto vbv = readOriginalVbv(
        setting.getIntVideoFilePath(key.video), reformInfo, plan);
    if (!vbv.available) {
        ctx.warn(_T("[MPEG-2部分エンコード] 元sequence headerを取得できないためVBV指定を省略します"));
    }

    ScriptEnvironmentPointer env = make_unique_ptr(CreateScriptEnvironment2());
    const int decodeThreads = std::max(1, std::min(8, GetProcessorCount()));
    auto source = av::LoadAMTSourceDirect(
        ctx, setting.getTmpAMTSourcePath(key.video), decodeThreads, env.get());
    const auto vi = source->GetVideoInfo();
    if (vi.num_frames != (int)displayFrames.size()) {
        THROWF(FormatException, "AMTSourceと表示順フレーム数が一致しません: %d/%d",
            vi.num_frames, (int)displayFrames.size());
    }
    for (size_t i = 0; i < plan.patches.size(); ++i) {
        const auto& patch = plan.patches[i];
        const int firstDts = reformInfo.getDtsFrameIndex(
            displayFrames[patch.first].frameIndex);
        const bool bottomFieldFirst =
            reformInfo.getVideoFrameInfo(firstDts).pic == PIC_BFF;
        const tstring patchPath = strsprintf(
            _T("%s.patch%d.m2v"), outputPath.c_str(), (int)i);
        ctx.registerTmpFile(patchPath);
        const int frames = patch.last - patch.first;
        ctx.infoF(_T("[MPEG-2部分エンコード] patch %d/%d: 表示順 [%d,%d) %dフレーム"),
            (int)i + 1, (int)plan.patches.size(), patch.first, patch.last, frames);
        // 第5引数disablePowerThrottoling=trueは固定。
        // 部分エンコードのpatchは必ずx262(=CPUエンコーダ)なので、
        // TranscodeManager側の判定(x264/x262/x265/SVT-AV1ならtrue)と一致する。
        // なお最終引数sarInContainerOnly=false(既定)のため、Y4Mヘッダには実SAR(例 A4:3)が載る。
        // 一方コマンドラインの--sarにはDARを渡している(x262のMPEG-2時の仕様、§8.2/§16.8)。
        // 両者の値は食い違うが、x262はCLI指定を優先するので意図どおり動く。
        Y4MEncodeWriter writer(ctx,
            makePatchEncoderArgs(setting, format, patchPath, frames, bottomFieldFirst, vbv),
            vi, format, true);
        for (int display = patch.first; display < patch.last; ++display) {
            writer.inputFrame(source->GetFrame(display, env.get()));
        }
        writer.finish();
        removeSequenceEndCode(patchPath);
        result.push_back(readPatchPictures(patchPath, frames));
    }
    return result;
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
                THROWF(FormatException, "出力TSのpicture byte列が不一致です: packet=%d", (int)index);
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
    AMTContext& ctx,
    const tstring& inputPath,
    const tstring& outputPath,
    const StreamReformInfo& reformInfo,
    const Mpeg2PartialEncodePlan& plan,
    const std::vector<PatchEncodedData>& patchData) {
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
    std::unique_ptr<AVPacket, PacketDeleter> patchPacket(av_packet_alloc());
    if (!packet || !patchPacket) {
        THROW(RuntimeException, "AVPacketを確保できません");
    }

    size_t outputEntryIndex = 0;
    auto writePatchEntries = [&]() {
        while (outputEntryIndex < plan.outputEntries.size()
            && plan.outputEntries[outputEntryIndex].kind == Mpeg2PartialAction::PATCH) {
            const auto& entry = plan.outputEntries[outputEntryIndex];
            if (entry.patchIndex < 0 || entry.patchIndex >= (int)patchData.size()
                || entry.patchPicture < 0
                || entry.patchPicture >= (int)patchData[entry.patchIndex].pictures.size()) {
                THROW(FormatException, "patch picture参照が範囲外です");
            }
            const auto& data = patchData[entry.patchIndex].pictures[entry.patchPicture];
            av_packet_unref(patchPacket.get());
            checkAv(av_new_packet(patchPacket.get(), (int)data.size()),
                "patch AVPacketを確保できません");
            std::memcpy(patchPacket->data, data.data(), data.size());
            patchPacket->stream_index = outputStream->index;
            patchPacket->pts = av_rescale_q(entry.pts90k, CLOCK_90K, outputStream->time_base);
            patchPacket->dts = av_rescale_q(entry.dts90k, CLOCK_90K, outputStream->time_base);
            patchPacket->duration = 0;
            patchPacket->pos = -1;
            expectedOutput.push_back({
                entry.pts90k, entry.dts90k, (int)data.size(), fnv1a(data.data(), (int)data.size())
            });
            checkAv(av_write_frame(output.get(), patchPacket.get()),
                "MPEG-TS patch packetを書けません");
            ++outputEntryIndex;
        }
    };

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

        writePatchEntries();
        if (plan.actions[filePacketIndex] == Mpeg2PartialAction::COPY) {
            if (outputEntryIndex >= plan.outputEntries.size()
                || plan.outputEntries[outputEntryIndex].kind != Mpeg2PartialAction::COPY
                || plan.outputEntries[outputEntryIndex].localDts != filePacketIndex) {
                THROW(FormatException, "COPY pictureと出力符号化順のmappingが不一致です");
            }
            const auto& entry = plan.outputEntries[outputEntryIndex];
            const auto hash = fnv1a(packet->data, packet->size);
            packet->stream_index = outputStream->index;
            packet->pts = av_rescale_q(entry.pts90k, CLOCK_90K, outputStream->time_base);
            packet->dts = av_rescale_q(entry.dts90k, CLOCK_90K, outputStream->time_base);
            // DROP前の間隔を持ち越さず、timestampだけを時刻情報の権威とする。
            packet->duration = 0;
            packet->pos = -1;
            expectedOutput.push_back({
                entry.pts90k, entry.dts90k, packet->size, hash
            });
            checkAv(av_write_frame(output.get(), packet.get()), "MPEG-TS packetを書けません");
            ++outputEntryIndex;
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
    writePatchEntries();
    if (outputEntryIndex != plan.outputEntries.size()) {
        THROWF(FormatException, "出力符号化順entryを処理しきれません: %d/%d",
            (int)outputEntryIndex, (int)plan.outputEntries.size());
    }
    checkAv(av_write_trailer(output.get()), "MPEG-TS trailerを書けません");
    if (output->pb != nullptr) {
        checkAv(avio_closep(&output->pb), "MPEG-TS出力を閉じられません");
    }
    output.reset();
    input.reset();

    verifyOutput(outputPath, expectedOutput);
    ctx.infoF(_T("[MPEG-2部分エンコード] 再demux検証: %d picture、PTS/DTS・size・hash不一致0"),
        (int)expectedOutput.size());
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
        const auto basePicture = frameAtDisplay(patch.first).pic;
        // 注: ここでhalfDelayを弾いてはいけない。
        // halfDelayはPIC_BFF(とPIC_BFF_RFFの前半)に必ず立つ(StreamReform.cpp)ので、
        // 条件に入れるとBFF素材のpatchが全部フォールバックしてしまう。
        // halfDelayによるpts半フレームずらしは出力PTS算出に影響しない
        // (出力PTSはptsではなくoriginalFramePTSを使うため、§9.3)。
        for (int display = patch.first; display < patch.last; ++display) {
            const auto& source = frameAtDisplay(display);
            if (isPatchUnsupportedPicture(source.pic)) {
                reason = _T("patch範囲にRFF付きフレームがあります");
                return false;
            }
            if (source.format != baseFormat) {
                reason = _T("patch範囲に映像フォーマット変化があります");
                return false;
            }
            if (source.pic != basePicture) {
                reason = _T("patch範囲でprogressive/TFF/BFFが混在しています");
                return false;
            }
        }
        const bool pictureMatchesFormat = baseFormat.progressive
            ? basePicture == PIC_FRAME
            : (basePicture == PIC_TFF || basePicture == PIC_BFF);
        if (!pictureMatchesFormat) {
            reason = _T("patch範囲のprogressive/TFF/BFFと映像フォーマットが一致しません");
            return false;
        }
    }

    plan.dtsFrameStart = frameRange.first;
    plan.actions.assign(codedFrameCount, Mpeg2PartialAction::DROP);
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

    // patchに置き換えるpictureはCOPY対象から外す。
    for (int localDts = 0; localDts < codedFrameCount; ++localDts) {
        if (patchState[localDts] == 1) {
            keepState[localDts] = 0;
        }
    }

    for (int localDts = 0; localDts < codedFrameCount; ++localDts) {
        if (keepState[localDts] == 1) {
            plan.actions[localDts] = Mpeg2PartialAction::COPY;
        }
    }

    // COPYをDTS順に走査し、表示範囲を追い越す直前へpatchをまとめて挿入する。
    size_t patchIndex = 0;
    for (int localDts = 0; localDts < codedFrameCount; ++localDts) {
        if (plan.actions[localDts] != Mpeg2PartialAction::COPY) {
            continue;
        }
        while (patchIndex < plan.patches.size()
            && firstDisplay[localDts] >= plan.patches[patchIndex].last) {
            const auto& patch = plan.patches[patchIndex];
            for (int display = patch.first; display < patch.last; ++display) {
                plan.outputEntries.push_back({
                    Mpeg2PartialAction::PATCH, -1, (int)patchIndex, display - patch.first,
                    outputDisplayPts[display], 0
                });
            }
            ++patchIndex;
        }
        const int display = firstDisplay[localDts];
        if (display < 0 || outputDisplayPts[display] == AV_NOPTS_VALUE) {
            reason = _T("COPY pictureの表示順PTSを取得できません");
            return false;
        }
        plan.outputEntries.push_back({
            Mpeg2PartialAction::COPY, localDts, -1, -1, outputDisplayPts[display], 0
        });
    }
    while (patchIndex < plan.patches.size()) {
        const auto& patch = plan.patches[patchIndex];
        for (int display = patch.first; display < patch.last; ++display) {
            plan.outputEntries.push_back({
                Mpeg2PartialAction::PATCH, -1, (int)patchIndex, display - patch.first,
                outputDisplayPts[display], 0
            });
        }
        ++patchIndex;
    }

    std::vector<int64_t> displayOrderPicturePts;
    displayOrderPicturePts.reserve(plan.outputEntries.size());
    for (const auto& entry : plan.outputEntries) {
        displayOrderPicturePts.push_back(entry.pts90k);
    }
    std::sort(displayOrderPicturePts.begin(), displayOrderPicturePts.end());
    if (displayOrderPicturePts.empty()) {
        reason = _T("出力pictureがありません");
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
    int64_t previousDts = std::numeric_limits<int64_t>::min();
    const int64_t firstDts = displayOrderPicturePts.front() - frameDuration;
    if (firstDts < 0) {
        reason = _T("出力DTSが負になるためフル再エンコードへフォールバックします");
        return false;
    }
    for (size_t outputIndex = 0; outputIndex < plan.outputEntries.size(); ++outputIndex) {
        auto& entry = plan.outputEntries[outputIndex];
        const int64_t dts = outputIndex == 0
            ? displayOrderPicturePts.front() - frameDuration
            : displayOrderPicturePts[outputIndex - 1];
        entry.dts90k = dts;
        if (dts <= previousDts || dts > entry.pts90k) {
            reason = _T("出力DTSの単調性またはDTS <= PTS条件を満たしません");
            return false;
        }
        previousDts = dts;
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
        ctx.infoF(_T("[MPEG-2部分エンコード] プラン: patch %d区間、COPY %d picture、出力 %d picture、keep %d表示位置"),
            (int)plan.patches.size(),
            (int)std::count(plan.actions.begin(), plan.actions.end(), Mpeg2PartialAction::COPY),
            (int)plan.outputEntries.size(),
            (int)reformInfo.getEncodeFile(key).videoFrames.size());
        const auto patchData = encodePatches(
            ctx, setting, reformInfo, key, plan, outputPath);
        buildOutput(ctx, setting.getIntVideoFilePath(key.video), outputPath,
            reformInfo, plan, patchData);
        ctx.infoF(_T("[MPEG-2部分エンコード] %d pictureを出力しました"),
            (int)plan.outputEntries.size());
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
