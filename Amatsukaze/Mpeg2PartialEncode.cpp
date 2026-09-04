#include "Mpeg2PartialEncode.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "AMTSource.h"
#include "Encoder.h"
#include "OSUtil.h"
#include "ReaderWriterFFmpeg.h"
#include "StreamReform.h"
#include "TranscodeSetting.h"
#include "rgy_filesystem.h"
#include "rgy_queue.h"

namespace {

constexpr AVRational CLOCK_90K = { 1, MPEG_CLOCK_HZ };
constexpr int64_t TIMESTAMP_MASK = (int64_t{ 1 } << 33) - 1;
constexpr int MIN_COPY_FRAMES = 30;
constexpr int PROGRESS_LOG_INTERVAL_SECONDS = 1;

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

struct InputContextDeleter {
    void operator()(AVFormatContext* format) const {
        avformat_close_input(&format);
    }
};

struct OutputContextDeleter {
    void operator()(AVFormatContext* format) const {
        if (format != nullptr) {
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
    for (int i = 0; i < size; i++) {
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

// picture typeがインタレ(フィールド順序を持つ)かどうか。
// 注: これはpatchエンコードのフィールドオーダー選択には使わない。
// AMTSourceはhalfDelayエントリを「前フレームのtop + 当該フレームのbottom」で
// 合成するため、表示エントリはPIC_BFF/PIC_BFF_RFFでも常にTFF順になる。
// 通常経路も同じ表示エントリを常に--tff/Y4M "It"でエンコードしている
// (TranscodeSetting.cpp、Encoder.cpp Y4MWriter)。§19.7参照。
bool isInterlacedPicture(PICTURE_TYPE picture) {
    return picture == PIC_TFF || picture == PIC_BFF
        || picture == PIC_TFF_RFF || picture == PIC_BFF_RFF;
}

std::vector<KeepRun> makeKeepRuns(const EncodeFileInput& file, int numDisplayFrames) {
    std::vector<KeepRun> runs;
    if (file.videoFrames.empty()) {
        THROW(FormatException, "出力対象の映像フレームがありません");
    }
    for (size_t i = 0; i < file.videoFrames.size(); i++) {
        const int frame = file.videoFrames[i];
        if (frame < 0 || frame >= numDisplayFrames
            || (i > 0 && frame <= file.videoFrames[i - 1])) {
            THROW(FormatException, "keep表示順フレーム列が不正です");
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
        const bool hasShortcopyGap = !merged.empty()
            && patch.first >= merged.back().last
            && patch.first - merged.back().last < MIN_COPY_FRAMES
            && std::all_of(keepMask.begin() + merged.back().last,
                keepMask.begin() + patch.first, [](bool keep) { return keep; });
        if (overlaps || hasShortcopyGap) {
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
    // av_find_input_formatの戻り型はlavf59以降const付き、nekopanda版(58)はconst無し。
    // auto*でどちらのAPIでもそのままavformat_open_inputに渡せるようにする。
    auto* inputFormat = av_find_input_format(demuxer);
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
    int frames) {
    if (format.format != VS_MPEG2) {
        THROW(FormatException, "MPEG-2以外の映像フォーマットからpatchエンコーダ引数を生成できません");
    }
    const uint8_t profileAndLevel = format.mpeg2ProfileAndLevelIndication;
    if ((profileAndLevel & 0x80) != 0) {
        THROW(FormatException, "カット境界再エンコードは4:2:2 Profileに対応していません");
    }
    const tchar* profile = nullptr;
    switch ((profileAndLevel >> 4) & 0x07) {
    case 1: profile = _T("high"); break;
    case 4: profile = _T("main"); break;
    case 5: profile = _T("simple"); break;
    default:
        THROWF(FormatException,
            "カット境界再エンコードはprofile_and_level_indication=0x%02XのProfileに対応していません",
            profileAndLevel);
    }
    const tchar* level = nullptr;
    switch (profileAndLevel & 0x0f) {
    case 0x0a: level = _T("low"); break;
    case 0x08: level = _T("main"); break;
    case 0x06: level = _T("high-1440"); break;
    case 0x04: level = _T("high"); break;
    case 0x02: level = _T("highp"); break;
    default:
        THROWF(FormatException,
            "カット境界再エンコードはprofile_and_level_indication=0x%02XのLevelに対応していません",
            profileAndLevel);
    }
    const int maxrateKbps = (int)(
        ((uint64_t)format.mpeg2BitRateValue * 400 + 999) / 1000);
    const int bufferKbit = (int)(
        ((uint64_t)format.mpeg2VbvBufferSizeValue * 16 * 1024 + 999) / 1000);

    StringBuilderT sb;
    sb.append(_T("\"%s\" --mpeg2 --demuxer y4m"), setting.getEncoderPath())
        .append(_T(" --profile %s --level %s"), profile, level)
        .append(_T(" --keyint 15 --bframes 0 --min-keyint 1 --scenecut 0"));
    if (!format.progressive) {
        // 表示エントリは常にTFF順(isInterlacedPictureのコメント参照)。
        // Y4MWriterもprogressiveでなければ"It"を書くので、ここも--tff固定。
        sb.append(_T(" --tff"));
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
    sb.append(_T(" --crf 6 --vbv-maxrate %d --vbv-bufsize %d"), maxrateKbps, bufferKbit);
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

    ScriptEnvironmentPointer env = make_unique_ptr(CreateScriptEnvironment2());
    const int decodeThreads = std::max(1, std::min(8, GetProcessorCount()));
    auto source = av::LoadAMTSourceDirect(
        ctx, setting.getTmpAMTSourcePath(key.video), decodeThreads, env.get());
    const auto vi = source->GetVideoInfo();
    if (vi.num_frames != (int)displayFrames.size()) {
        THROWF(FormatException, "AMTSourceと表示順フレーム数が一致しません: %d/%d",
            vi.num_frames, (int)displayFrames.size());
    }
    for (size_t i = 0; i < plan.patches.size(); i++) {
        const auto& patch = plan.patches[i];
        const int firstDts = reformInfo.getDtsFrameIndex(
            displayFrames[patch.first].frameIndex);
        const auto& sourceFrame = reformInfo.getVideoFrameInfo(firstDts);
        const tstring patchPath = strsprintf(
            _T("%s.patch%d.m2v"), outputPath.c_str(), (int)i);
        ctx.registerTmpFile(patchPath);
        const int frames = patch.last - patch.first;
        ctx.infoF(_T("[カット境界再エンコード] patch %d/%d: 表示順 [%d,%d) %dフレーム"),
            (int)i + 1, (int)plan.patches.size(), patch.first, patch.last, frames);

        // progressive_sequence=1 なら frame picture は PIC_FRAME/DOUBLING/TRIPLING に
        // しかならず(Mpeg2VideoParser.cpp)、PIC_TFF/PIC_BFF が出るのは規格上禁止の
        // field picture のときだけ。それでも混ざっていたらエラーにはせず、
        // patch範囲をインタレとしてエンコードして吸収する(§20)。
        // Y4Mヘッダ側もインタレ("It")に揃える必要があるのでformatごと差し替える。
        VideoFormat patchFormat = sourceFrame.format;
        VideoFormat writerFormat = format;
        if (patchFormat.progressive) {
            bool hasInterlaced = false;
            for (int display = patch.first; display < patch.last && !hasInterlaced; display++) {
                const int dts = reformInfo.getDtsFrameIndex(displayFrames[display].frameIndex);
                hasInterlaced = isInterlacedPicture(reformInfo.getVideoFrameInfo(dts).pic);
            }
            if (hasInterlaced) {
                ctx.warnF(_T("[カット境界再エンコード] patch %d/%d: progressive映像にインタレpictureが混在するため、patch範囲をインタレとしてエンコードします"),
                    (int)i + 1, (int)plan.patches.size());
                patchFormat.progressive = false;
                writerFormat.progressive = false;
            }
        }
        const tstring encoderArgs = makePatchEncoderArgs(
            setting, patchFormat, patchPath, frames);
        ctx.info(_T("[エンコーダ起動]"));
        ctx.infoF(_T("%s"), encoderArgs.c_str());
        // 第5引数disablePowerThrottoling=trueは固定。
        // カット境界再エンコードのpatchは必ずx262(=CPUエンコーダ)なので、
        // TranscodeManager側の判定(x264/x262/x265/SVT-AV1ならtrue)と一致する。
        // なお最終引数sarInContainerOnly=false(既定)のため、Y4Mヘッダには実SAR(例 A4:3)が載る。
        // 一方コマンドラインの--sarにはDARを渡している(x262のMPEG-2時の仕様、§8.2/§16.8)。
        // 両者の値は食い違うが、x262はCLI指定を優先するので意図どおり動く。
        Y4MEncodeWriter writer(ctx, encoderArgs, vi, writerFormat, true);
        try {
            for (int display = patch.first; display < patch.last; display++) {
                writer.inputFrame(source->GetFrame(display, env.get()));
            }
        } catch (...) {
            // 子プロセスが入力途中で終了するとinputFrameが例外を投げる。
            // ~Y4MEncodeWriter()は稼働中なら例外を投げるので、そのまま巻き戻すと
            // 例外送出中のデストラクタ例外でterminateする。元の例外を投げ直す前に
            // 必ずfinish() (finishWrite + join) まで済ませること。
            // finish()自体が投げた場合はエンコーダ終了コードのほうが原因として
            // 有用なので、そちらを優先して伝播させる。
            writer.finish();
            throw;
        }
        writer.finish();
        removeSequenceEndCode(patchPath);
        result.push_back(readPatchPictures(patchPath, frames));
    }
    return result;
}

class PacketExpectationQueue {
public:
    void push(PacketExpectation item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (finished_ || cancelled_) {
            THROW(InvalidOperationException, "出力検証の期待値キューは終了しています");
        }
        queue_.push_back(std::move(item));
        condition_.notify_one();
    }

    bool pop(PacketExpectation& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() {
            return !queue_.empty() || finished_ || cancelled_;
        });
        if (cancelled_ || queue_.empty()) {
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        condition_.notify_all();
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        condition_.notify_all();
    }

private:
    std::deque<PacketExpectation> queue_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool finished_ = false;
    bool cancelled_ = false;
};

class QueueInputContext {
public:
    explicit QueueInputContext(RGYQueueBuffer& queue)
        : queue_(queue) {
        constexpr int BUFFER_SIZE = 128 * 1024;
        auto* buffer = (uint8_t*)av_malloc(BUFFER_SIZE);
        if (buffer == nullptr) {
            THROW(RuntimeException, "出力検証用AVIOバッファを確保できません");
        }
        context_ = avio_alloc_context(
            buffer, BUFFER_SIZE, 0, this, &QueueInputContext::readPacket, nullptr, nullptr);
        if (context_ == nullptr) {
            av_free(buffer);
            THROW(RuntimeException, "出力検証用AVIOContextを確保できません");
        }
#if LIBAVFORMAT_VERSION_MAJOR < 59
        // 旧FFmpegのmpegts demuxerは非seekable入力のプローブ時にAVIOバッファを拡張するが、
        // その後の縮小処理に不具合がありfill_buffer()内のassertへ到達するため縮小を無効化する。
        context_->orig_buffer_size = 0;
#endif
        context_->seekable = 0;
    }

    ~QueueInputContext() {
        if (context_ != nullptr) {
            av_freep(&context_->buffer);
            avio_context_free(&context_);
        }
    }

    AVIOContext* get() const {
        return context_;
    }

private:
    static int readPacket(void* opaque, uint8_t* buffer, int bufferSize) {
        auto* self = static_cast<QueueInputContext*>(opaque);
        const int64_t readSize = self->queue_.popDataBlock(buffer, bufferSize);
        return readSize < 0 ? AVERROR_EOF : (int)readSize;
    }

    RGYQueueBuffer& queue_;
    AVIOContext* context_ = nullptr;
};

class StreamingOutputVerifier {
public:
    explicit StreamingOutputVerifier(size_t totalPictures)
        : totalPictures_(totalPictures) {
        constexpr int64_t INITIAL_QUEUE_SIZE = 4 * 1024 * 1024;
        constexpr int64_t MAX_QUEUE_SIZE = 256 * 1024 * 1024;
        tsQueue_.init(INITIAL_QUEUE_SIZE);
        // 検証側の一時的な遅れは十分吸収しつつ、異常時の無制限な増加は防ぐ。
        tsQueue_.setMaxCapacity(MAX_QUEUE_SIZE);
        thread_ = std::thread([this]() { run(); });
    }

    ~StreamingOutputVerifier() {
        cancel();
        join();
    }

    void pushTs(const uint8_t* data, int size) {
        while (!tsQueue_.pushData(data, size, 100)) {
            // 検証スレッドが停止していた場合、容量待ちを続けず元の例外を返す。
            rethrowError();
        }
        rethrowError();
    }

    void pushExpectation(PacketExpectation item) {
        rethrowError();
        expectations_.push(std::move(item));
    }

    void finish() {
        expectations_.finish();
        tsQueue_.setEOF();
        join();
        rethrowError();
    }

    void cancel() {
        expectations_.cancel();
        tsQueue_.setEOF();
    }

    size_t verifiedPictures() const {
        return verifiedPictures_.load();
    }

    void rethrowError() const {
        if (!failed_.load()) {
            return;
        }
        std::lock_guard<std::mutex> lock(errorMutex_);
        if (error_ != nullptr) {
            std::rethrow_exception(error_);
        }
        THROW(RuntimeException, "出力のストリーム検証に失敗しました");
    }

private:
    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void run() noexcept {
        try {
            QueueInputContext inputIo(tsQueue_);
            AVFormatContext* rawInput = avformat_alloc_context();
            if (rawInput == nullptr) {
                THROW(RuntimeException, "出力検証用AVFormatContextを確保できません");
            }
            rawInput->pb = inputIo.get();
            rawInput->flags |= AVFMT_FLAG_CUSTOM_IO;
            auto* inputFormat = av_find_input_format("mpegts");
            if (inputFormat == nullptr) {
                avformat_free_context(rawInput);
                THROW(FormatException, "mpegts demuxerが見つかりません");
            }
            const int openResult = avformat_open_input(
                &rawInput, nullptr, inputFormat, nullptr);
            if (openResult < 0) {
                if (rawInput != nullptr) {
                    avformat_free_context(rawInput);
                }
                checkAv(openResult, "メモリ上の出力TSを開けません");
            }
            std::unique_ptr<AVFormatContext, InputContextDeleter> input(rawInput);
            // 非seekableなcustom AVIOでavformat_find_stream_info()を呼ぶと、旧FFmpegでは
            // プローブ用バッファの拡大・縮小時にassertへ到達するため、packetから逐次判定する。
            int videoIndex = -1;

            std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
            if (!packet) {
                THROW(RuntimeException, "出力検証用AVPacketを確保できません");
            }
            int readResult = 0;
            while ((readResult = av_read_frame(input.get(), packet.get())) >= 0) {
                AVStream* packetStream = input->streams[packet->stream_index];
                if (videoIndex < 0
                    && packetStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    videoIndex = packet->stream_index;
                }
                if (packet->stream_index == videoIndex) {
                    PacketExpectation item = {};
                    if (!expectations_.pop(item)) {
                        THROW(FormatException, "出力TSのpicture数が期待値より多いです");
                    }
                    const size_t index = verifiedPictures_.load();
                    const int64_t pts = packet->pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
                        : av_rescale_q(packet->pts, packetStream->time_base, CLOCK_90K);
                    const int64_t dts = packet->dts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
                        : av_rescale_q(packet->dts, packetStream->time_base, CLOCK_90K);
                    if (!timestampEquals(pts, item.pts) || !timestampEquals(dts, item.dts)) {
                        THROWF(FormatException,
                            "出力TSのPTS/DTSが不一致です: packet=%d pts=%lld/%lld dts=%lld/%lld",
                            (int)index, (long long)pts, (long long)item.pts,
                            (long long)dts, (long long)item.dts);
                    }
                    if (packet->size != item.size
                        || fnv1a(packet->data, packet->size) != item.hash) {
                        THROWF(FormatException,
                            "出力TSのpicture byte列が不一致です: packet=%d", (int)index);
                    }
                    verifiedPictures_.fetch_add(1);
                }
                av_packet_unref(packet.get());
            }
            if (readResult != AVERROR_EOF) {
                checkAv(readResult, "メモリ上の出力TSのpacket読み出しに失敗しました");
            }
            if (videoIndex < 0) {
                THROW(FormatException, "メモリ上の出力TSに映像ストリームがありません");
            }
            if (verifiedPictures_.load() != totalPictures_) {
                THROWF(FormatException, "出力TSのpicture数が不一致です: %d/%d",
                    (int)verifiedPictures_.load(), (int)totalPictures_);
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(errorMutex_);
                error_ = std::current_exception();
            }
            failed_.store(true);
            expectations_.cancel();
        }
    }

    size_t totalPictures_;
    RGYQueueBuffer tsQueue_;
    PacketExpectationQueue expectations_;
    std::thread thread_;
    std::atomic<size_t> verifiedPictures_ = 0;
    std::atomic<bool> failed_ = false;
    mutable std::mutex errorMutex_;
    std::exception_ptr error_;
};

class Mpeg2PartialOutputIO {
public:
    Mpeg2PartialOutputIO(const tstring& outputPath, size_t totalPictures)
        : file_(outputPath, _T("wb"))
        , verifier_(totalPictures) {
        constexpr int BUFFER_SIZE = 128 * 1024;
        auto* buffer = (uint8_t*)av_malloc(BUFFER_SIZE);
        if (buffer == nullptr) {
            THROW(RuntimeException, "カット境界再エンコード出力用AVIOバッファを確保できません");
        }
        context_ = avio_alloc_context(
            buffer, BUFFER_SIZE, 1, this, nullptr,
            (RGYArgN<5, decltype(avio_alloc_context)>::type)(&Mpeg2PartialOutputIO::writePacket),
            nullptr);
        if (context_ == nullptr) {
            av_free(buffer);
            THROW(RuntimeException, "カット境界再エンコード出力用AVIOContextを確保できません");
        }
        context_->seekable = 0;
    }

    ~Mpeg2PartialOutputIO() {
        if (!finished_) {
            verifier_.cancel();
        }
        if (context_ != nullptr) {
            av_freep(&context_->buffer);
            avio_context_free(&context_);
        }
    }

    AVIOContext* get() const {
        return context_;
    }

    void pushExpectation(PacketExpectation item) {
        verifier_.pushExpectation(std::move(item));
    }

    size_t verifiedPictures() const {
        return verifier_.verifiedPictures();
    }

    void checkResult(int result, const char* operation) const {
        rethrowWriteError();
        verifier_.rethrowError();
        checkAv(result, operation);
    }

    void finish() {
        avio_flush(context_);
        checkResult(context_->error, "カット境界再エンコード出力をflushできません");
        file_.flush();
        verifier_.finish();
        finished_ = true;
    }

private:
    static int writePacket(void* opaque, uint8_t* buffer, int bufferSize) {
        return static_cast<Mpeg2PartialOutputIO*>(opaque)->write(buffer, bufferSize);
    }

    int write(uint8_t* buffer, int bufferSize) noexcept {
        try {
            verifier_.rethrowError();
            file_.write(MemoryChunk(buffer, bufferSize));
            verifier_.pushTs(buffer, bufferSize);
            return bufferSize;
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(writeErrorMutex_);
                if (writeError_ == nullptr) {
                    writeError_ = std::current_exception();
                }
            }
            verifier_.cancel();
            return AVERROR_EXTERNAL;
        }
    }

    void rethrowWriteError() const {
        std::lock_guard<std::mutex> lock(writeErrorMutex_);
        if (writeError_ != nullptr) {
            std::rethrow_exception(writeError_);
        }
    }

    File file_;
    StreamingOutputVerifier verifier_;
    AVIOContext* context_ = nullptr;
    bool finished_ = false;
    mutable std::mutex writeErrorMutex_;
    std::exception_ptr writeError_;
};

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

    Mpeg2PartialOutputIO outputIo(outputPath, plan.outputEntries.size());
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
    // AVFMT_AVOID_NEG_TS_DISABLED(=0)はFFmpeg 5.1で追加された名前で、
    // nekopanda版FFmpeg(lavf58)には無いが値0=補正無効の意味は同じ。
#ifndef AVFMT_AVOID_NEG_TS_DISABLED
#define AVFMT_AVOID_NEG_TS_DISABLED 0
#endif
    output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    output->pb = outputIo.get();
    output->flags |= AVFMT_FLAG_CUSTOM_IO;
    outputIo.checkResult(
        avformat_write_header(output.get(), nullptr), "MPEG-TSヘッダを書けません");

    ctx.infoF(_T("[カット境界再エンコード] 統合開始: 元映像と再エンコードpatchをMPEG-TSへ統合します (入力 %d picture、出力予定 %d picture)"),
        (int)plan.actions.size(), (int)plan.outputEntries.size());
    ctx.progressF(_T("[カット境界再エンコード] 統合中: 入力 0/%d picture (0.0%%)、出力 0/%d picture、検証 0/%d picture"),
        (int)plan.actions.size(), (int)plan.outputEntries.size(),
        (int)plan.outputEntries.size());

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
            PacketExpectation expected = {
                entry.pts90k, entry.dts90k, (int)data.size(), fnv1a(data.data(), (int)data.size())
            };
            outputIo.checkResult(av_write_frame(output.get(), patchPacket.get()),
                "MPEG-TS patch packetを書きこめません");
            outputIo.pushExpectation(std::move(expected));
            outputEntryIndex++;
        }
    };

    int filePacketIndex = 0;
    int readResult = 0;
    const auto mergeStart = std::chrono::steady_clock::now();
    auto nextProgressLog = mergeStart
        + std::chrono::seconds(PROGRESS_LOG_INTERVAL_SECONDS);
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
                THROW(FormatException, "copy pictureと出力符号化順のmappingが不一致です");
            }
            const auto& entry = plan.outputEntries[outputEntryIndex];
            const auto hash = fnv1a(packet->data, packet->size);
            packet->stream_index = outputStream->index;
            packet->pts = av_rescale_q(entry.pts90k, CLOCK_90K, outputStream->time_base);
            packet->dts = av_rescale_q(entry.dts90k, CLOCK_90K, outputStream->time_base);
            // DROP前の間隔を持ち越さず、timestampだけを時刻情報の権威とする。
            packet->duration = 0;
            packet->pos = -1;
            PacketExpectation expected = {
                entry.pts90k, entry.dts90k, packet->size, hash
            };
            outputIo.checkResult(
                av_write_frame(output.get(), packet.get()), "MPEG-TS packetを書けません");
            outputIo.pushExpectation(std::move(expected));
            outputEntryIndex++;
        }
        filePacketIndex++;
        av_packet_unref(packet.get());
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextProgressLog) {
            const double progress = plan.actions.empty()
                ? 100.0 : 100.0 * filePacketIndex / plan.actions.size();
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - mergeStart).count();
            ctx.progressF(_T("[カット境界再エンコード] 統合中: 入力 %d/%d picture (%.1f%%)、出力 %d/%d picture、検証 %d/%d picture、経過 %lld秒"),
                filePacketIndex, (int)plan.actions.size(), progress,
                (int)outputEntryIndex, (int)plan.outputEntries.size(),
                (int)outputIo.verifiedPictures(), (int)plan.outputEntries.size(),
                (long long)elapsed);
            nextProgressLog = now
                + std::chrono::seconds(PROGRESS_LOG_INTERVAL_SECONDS);
        }
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
    outputIo.checkResult(av_write_trailer(output.get()), "MPEG-TS trailerを書けません");
    outputIo.finish();
    output->pb = nullptr;
    output.reset();
    input.reset();

    const auto mergeElapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - mergeStart).count();
    ctx.infoF(_T("[カット境界再エンコード] 統合・検証完了: %d picture (経過 %lld秒)"),
        (int)outputEntryIndex, (long long)mergeElapsed);
    ctx.progressF(_T("[カット境界再エンコード] 統合・検証完了: 入力 %d/%d picture (100.0%%)、出力 %d/%d picture、検証 %d/%d picture"),
        (int)plan.actions.size(), (int)plan.actions.size(),
        (int)outputEntryIndex, (int)plan.outputEntries.size(),
        (int)outputIo.verifiedPictures(), (int)plan.outputEntries.size());
}

} // namespace

void BuildMpeg2PartialEncodePlan(
    const StreamReformInfo& reformInfo,
    EncodeFileKey key,
    Mpeg2PartialEncodePlan& plan) {
    plan = Mpeg2PartialEncodePlan();
    const auto& displayFrames = reformInfo.getFilterSourceFrames(key.video);
    const auto& file = reformInfo.getEncodeFile(key);
    const auto frameRange = reformInfo.getVideoFrameRange(key.video);
    const int codedFrameCount = frameRange.second - frameRange.first;
    if (displayFrames.empty() || codedFrameCount <= 0) {
        THROW(FormatException, "中間映像ファイルのフレームがありません");
    }

    const auto runs = makeKeepRuns(file, (int)displayFrames.size());
    const auto keepSegments = reformInfo.getKeepSegments(key);
    if (keepSegments.size() != runs.size()) {
        THROW(FormatException, "keep区間テーブルと表示順フレーム列が一致しません");
    }

    std::vector<bool> keepMask(displayFrames.size(), false);
    // copy pictureが使う出力PTS。符号化picture本来の表示開始時刻(originalFramePTS)基準。
    std::vector<int64_t> outputDisplayPts(displayFrames.size(), AV_NOPTS_VALUE);
    // patchが使う表示グリッド。RFFでは1符号化pictureが複数の表示エントリに展開され、
    // originalFramePTSが重複するので、patchはこちら(等間隔のpts)に載せる(§19.7)。
    std::vector<double> displayGridPts(displayFrames.size(), 0);
    double cumulativeCut = 0;
    for (size_t i = 0; i < runs.size(); i++) {
        const auto& run = runs[i];
        const auto& segment = keepSegments[i];
        const int64_t expectedStart = (int64_t)std::llround(displayFrames[run.first].pts);
        const int64_t expectedEnd = (int64_t)std::llround(
            displayFrames[run.last - 1].pts + displayFrames[run.last - 1].frameDuration);
        if ((int64_t)std::llround(segment.start) != expectedStart
            || (int64_t)std::llround(segment.end) != expectedEnd) {
            THROW(FormatException, "keep区間テーブルのPTSが表示順フレーム列と一致しません");
        }
        if (i > 0) {
            cumulativeCut += segment.start - keepSegments[i - 1].end;
        }
        for (int display = run.first; display < run.last; display++) {
            keepMask[display] = true;
            outputDisplayPts[display] = (int64_t)std::llround(
                displayFrames[display].originalFramePTS - cumulativeCut);
            displayGridPts[display] = displayFrames[display].pts - cumulativeCut;
        }
    }

    // 表示エントリ → 中間映像ファイル内のDTS index。以降で繰り返し使うので先に作る。
    std::vector<int> displayLocalDts(displayFrames.size(), -1);
    for (int display = 0; display < (int)displayFrames.size(); display++) {
        const int globalDts = reformInfo.getDtsFrameIndex(displayFrames[display].frameIndex);
        const int localDts = globalDts - frameRange.first;
        if (localDts < 0 || localDts >= codedFrameCount) {
            THROW(FormatException, "表示順からDTS順へのmappingが中間映像ファイル範囲外です");
        }
        displayLocalDts[display] = localDts;
    }

    auto frameAtDisplay = [&](int display) -> const VideoFrameInfo& {
        return reformInfo.getVideoFrameInfo(displayLocalDts[display] + frameRange.first);
    };

    // 符号化picture単位でkeep/dropを集計する。
    // RFF (PIC_BFF_RFF) やPIC_FRAME_DOUBLING/TRIPLINGでは1符号化pictureが複数の
    // 表示エントリに展開されるため、カット点がその途中に落ちてkeepとdropに
    // 割れることがある。3:2プルダウンなら4符号化picture→5表示エントリで、
    // エントリ間5箇所のうち1箇所がPIC_BFF_RFFの内側なので約20%の確率で起きる。
    // 割れたpictureをそのままcopyするとdropすべき表示エントリまで出力されるので、
    // 必ずpatch側で扱う。§20参照。
    std::vector<uint8_t> codedKeepFlags(codedFrameCount, 0); // bit0: keepあり、bit1: dropあり
    for (int display = 0; display < (int)displayFrames.size(); display++) {
        codedKeepFlags[displayLocalDts[display]] |= keepMask[display] ? 1 : 2;
    }
    // その表示エントリの符号化pictureが、全表示エントリをkeepしているか。
    auto isFullyKept = [&](int display) {
        return codedKeepFlags[displayLocalDts[display]] == 1;
    };
    plan.splitPictures =
        (int)std::count(codedKeepFlags.begin(), codedKeepFlags.end(), (uint8_t)3);

    // 各カット境界の左右で必要になるpatch範囲を表示順で求める。
    // restoreI / lastAnchor には isFullyKept を要求する。割れたpictureを
    // lastAnchorにすると、それを参照するB pictureの参照先がpatchに置き換わって
    // 壊れる。復帰点も同様に、割れたpictureをcopy開始点にするとdrop側の表示
    // エントリまで出てしまう。探索でこれらを飛ばせば、割れたpictureのkeep側
    // エントリは自動的にpatch範囲へ入る(§20)。
    for (const auto& run : runs) {
        if (run.first > 0) {
            int restore = run.first;
            while (restore < run.last
                && !(isRestoreI(frameAtDisplay(restore)) && isFullyKept(restore))) {
                restore++;
            }
            // restoreIが見つからなければkeep区間を丸ごとpatchにする(§6.4)。
            // 地デジ・BSは0.5秒ごとにGOP先頭Iが来るので、この区間は1GOP未満と短い。
            addPatch(plan.patches, run.first, restore);
        }
        if (run.last < (int)displayFrames.size()) {
            int anchor = run.last - 1;
            while (anchor >= run.first
                && !(isAnchor(frameAtDisplay(anchor)) && isFullyKept(anchor))) {
                --anchor;
            }
            // アンカーが見つからない場合も同様にkeep区間を丸ごとpatchにする。
            addPatch(plan.patches, anchor + 1, run.last);
        }
    }
    mergePatches(plan.patches, keepMask);

    for (const auto& patch : plan.patches) {
        // 注: ここでhalfDelayやTFF/BFFの混在を弾いてはいけない。
        // halfDelayはPIC_BFF(とPIC_BFF_RFFの前半)に必ず立つ(StreamReform.cpp)ので、
        // 条件に入れるとBFF素材のpatchが全部弾かれてしまう。
        // また3:2プルダウン素材はTFF系とBFF系のpictureが必ず交互に現れるため、
        // 混在を弾くとRFF素材のpatchが必ず失敗する。
        // 表示エントリはAMTSourceが常にTFF順へ正規化するので混在は問題ない
        // (isInterlacedPictureのコメント、§19.7)。
        // progressive映像にインタレpictureが混ざるケースはencodePatches側で
        // patch範囲をインタレ扱いにして吸収する(§20)。
        const auto baseFormat = frameAtDisplay(patch.first).format;
        for (int display = patch.first; display < patch.last; display++) {
            if (frameAtDisplay(display).format != baseFormat) {
                THROW(FormatException, "patch範囲に映像フォーマット変化があります");
            }
        }
    }

    plan.dtsFrameStart = frameRange.first;
    plan.actions.assign(codedFrameCount, Mpeg2PartialAction::DROP);
    std::vector<int> firstDisplay(codedFrameCount, -1);
    std::vector<int> keepState(codedFrameCount, -1); // 0: DROP、1: copy
    std::vector<bool> patchMask(displayFrames.size(), false);
    for (const auto& patch : plan.patches) {
        std::fill(patchMask.begin() + patch.first, patchMask.begin() + patch.last, true);
    }
    std::vector<int> patchState(codedFrameCount, -1); // 0: copy候補、1: patch
    for (int display = 0; display < (int)displayFrames.size(); display++) {
        const int localDts = displayLocalDts[display];
        if (firstDisplay[localDts] < 0) {
            firstDisplay[localDts] = display;
        }
        keepState[localDts] = (codedKeepFlags[localDts] & 1) ? 1 : 0;
        if (keepMask[display]) {
            const int currentPatchState = patchMask[display] ? 1 : 0;
            if (patchState[localDts] >= 0 && patchState[localDts] != currentPatchState) {
                // patch範囲の端はrestoreI/lastAnchor探索の結果であり、探索は
                // 符号化pictureの先頭/末尾の表示エントリでしか止まらないので、
                // 同一pictureがpatchとcopyに分かれることは構造上ありえない。
                THROW(FormatException, "同一符号化pictureがpatchとcopyに分かれています");
            }
            patchState[localDts] = currentPatchState;
        }
    }
    // keepとdropに割れたpictureは必ずpatch側で扱われていなければならない。
    // isFullyKeptによるrestoreI/lastAnchor探索でこれは保証される。
    for (int localDts = 0; localDts < codedFrameCount; localDts++) {
        if (codedKeepFlags[localDts] == 3 && patchState[localDts] != 1) {
            THROW(FormatException, "keepとdropに割れた符号化pictureがpatchに入っていません");
        }
    }

    // patchに置き換えるpictureはcopy対象から外す。
    for (int localDts = 0; localDts < codedFrameCount; localDts++) {
        if (patchState[localDts] == 1) {
            keepState[localDts] = 0;
        }
    }

    for (int localDts = 0; localDts < codedFrameCount; localDts++) {
        if (keepState[localDts] == 1) {
            plan.actions[localDts] = Mpeg2PartialAction::COPY;
        }
    }

    // copyをDTS順に走査し、表示範囲を追い越す直前へpatchをまとめて挿入する。
    size_t patchIndex = 0;
    // patchは表示エントリ単位で1 pictureを出力するので、PTSは等間隔の表示グリッド
    // (displayGridPts)に載せる。ただしそのままだとPIC_BFF系のhalfDelay分だけ
    // copy側(originalFramePTS基準)とずれるため、patch先頭のずれ量を全体に足して
    // 接合させる。RFF以外では両者の差がpatch内で一定なので、結果は
    // outputDisplayPtsと完全に一致する(既存挙動の非回帰)。
    // RFFでは1符号化pictureごとにhalfDelayが変わるため、patch終端とcopyの接合は
    // 最大半フレームずれる(§19.7)。
    //
    // anchorShiftの基準は「符号化pictureの先頭表示エントリ」に限る。
    // δ = originalFramePTS - pts は先頭エントリなら {0, +T/2} しか取らないが、
    // 継続エントリではPIC_BFF_RFFの2枚目が -T/2、PIC_FRAME_DOUBLINGの2枚目が -T、
    // TRIPLINGの3枚目が -2T と符号が反転する。keepとdropに割れたpictureのkeep側は
    // 継続エントリなので、そこを基準にするとpatch全体が最大2フレーム手前へずれて
    // 直前のcopy pictureとPTSが衝突する。copy側はfirstDisplay(=先頭エントリ)を
    // 使うので、基準を揃えれば隣接間隔は必ず T - T/2 = T/2 > 0 で閉じる(§20)。
    // 現行で成立しているケースではpatch.firstが必ず先頭エントリなので非回帰。
    auto pushPatchEntries = [&](size_t index) {
        const auto& patch = plan.patches[index];
        double anchorShift = 0;
        for (int display = patch.first; display < patch.last; display++) {
            if (display == 0 || displayLocalDts[display - 1] != displayLocalDts[display]) {
                anchorShift = (double)outputDisplayPts[display] - displayGridPts[display];
                break;
            }
        }
        for (int display = patch.first; display < patch.last; display++) {
            plan.outputEntries.push_back({
                Mpeg2PartialAction::PATCH, -1, (int)index, display - patch.first,
                (int64_t)std::llround(displayGridPts[display] + anchorShift), 0
            });
        }
    };

    for (int localDts = 0; localDts < codedFrameCount; localDts++) {
        if (plan.actions[localDts] != Mpeg2PartialAction::COPY) {
            continue;
        }
        while (patchIndex < plan.patches.size()
            && firstDisplay[localDts] >= plan.patches[patchIndex].last) {
            pushPatchEntries(patchIndex);
            patchIndex++;
        }
        const int display = firstDisplay[localDts];
        if (display < 0 || outputDisplayPts[display] == AV_NOPTS_VALUE) {
            THROW(FormatException, "copy pictureの表示順PTSを取得できません");
        }
        plan.outputEntries.push_back({
            Mpeg2PartialAction::COPY, localDts, -1, -1, outputDisplayPts[display], 0
        });
    }
    while (patchIndex < plan.patches.size()) {
        pushPatchEntries(patchIndex);
        patchIndex++;
    }

    std::vector<int64_t> displayOrderPicturePts;
    displayOrderPicturePts.reserve(plan.outputEntries.size());
    for (const auto& entry : plan.outputEntries) {
        displayOrderPicturePts.push_back(entry.pts90k);
    }
    std::sort(displayOrderPicturePts.begin(), displayOrderPicturePts.end());
    if (displayOrderPicturePts.empty()) {
        THROW(FormatException, "出力pictureがありません");
    }
    if (std::adjacent_find(displayOrderPicturePts.begin(), displayOrderPicturePts.end())
        != displayOrderPicturePts.end()) {
        THROW(FormatException, "出力pictureの表示順PTSが重複しています");
    }

    const auto& format = reformInfo.getFormat(key).videoFormat;
    const int64_t frameDuration = (int64_t)std::llround(
        format.frameRateDenom * (double)MPEG_CLOCK_HZ / format.frameRateNum);
    int64_t previousDts = std::numeric_limits<int64_t>::min();
    const int64_t firstDts = displayOrderPicturePts.front() - frameDuration;
    if (firstDts < 0) {
        // 元TSのPTSがほぼ0という極端な素材でのみ発生する。δシフトで回避する案は
        // Muxer側のカットリストで指定する先頭トリムPTSとの整合が取れないため
        // 不採用としており(§18 表11)、ここはエラー停止とする。
        THROW(FormatException, "出力DTSが負になります");
    }
    for (size_t outputIndex = 0; outputIndex < plan.outputEntries.size(); outputIndex++) {
        auto& entry = plan.outputEntries[outputIndex];
        const int64_t dts = outputIndex == 0
            ? displayOrderPicturePts.front() - frameDuration
            : displayOrderPicturePts[outputIndex - 1];
        entry.dts90k = dts;
        if (dts <= previousDts || dts > entry.pts90k) {
            THROW(FormatException, "出力DTSの単調性またはDTS <= PTS条件を満たしません");
        }
        previousDts = dts;
    }
    for (int display = 0; display < (int)displayFrames.size(); display++) {
        const bool copied =
            plan.actions[displayLocalDts[display]] == Mpeg2PartialAction::COPY;
        if (keepMask[display] != (copied || patchMask[display])
            || (copied && patchMask[display])) {
            THROW(FormatException, "keep表示位置とcopy/patchプランが1:1対応していません");
        }
    }
}

// フル再エンコードへのフォールバックは廃止した(§20)。
// 素材起因で起こりうる条件は全てカット境界再エンコード側で処理し、それ以外は
// 実装の不整合を意味するのでエラー停止させる。黙って全編再エンコードに
// 落ちると、遅くなった理由がユーザーから見えないため。
void RunMpeg2PartialEncode(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    EncodeFileKey key) {
    const tstring outputPath = setting.getEncVideoFilePath(key);
    try {
        Mpeg2PartialEncodePlan plan;
        BuildMpeg2PartialEncodePlan(reformInfo, key, plan);
        ctx.infoF(_T("[カット境界再エンコード] プラン: patch %d区間、copy %d picture、出力 %d picture、keep %d表示位置、境界割れpicture %d"),
            (int)plan.patches.size(),
            (int)std::count(plan.actions.begin(), plan.actions.end(), Mpeg2PartialAction::COPY),
            (int)plan.outputEntries.size(),
            (int)reformInfo.getEncodeFile(key).videoFrames.size(),
            plan.splitPictures);
        const auto patchData = encodePatches(
            ctx, setting, reformInfo, key, plan, outputPath);
        buildOutput(ctx, setting.getIntVideoFilePath(key.video), outputPath,
            reformInfo, plan, patchData);
        ctx.infoF(_T("[カット境界再エンコード] %d pictureを出力しました"),
            (int)plan.outputEntries.size());
    } catch (...) {
        // 中途半端な出力ファイルを残さない。
        if (File::exists(outputPath)) {
            rgy_file_remove(outputPath.c_str());
        }
        throw;
    }
}
