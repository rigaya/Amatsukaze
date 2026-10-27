/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "TsSplitter.h"
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

struct AudioPesPipeline::Impl {
    struct Frame {
        AudioFrameData info;
        std::vector<uint8_t> coded;
        std::vector<uint16_t> decoded;
    };
    struct Result {
        std::vector<Frame> frames;
        AudioDecodeDiagnostics diagnostics;
    };
    struct Job {
        int audioIdx;
        int64_t clock;
        AudioComponentInfo component;
        std::vector<uint8_t> packet;
        std::promise<Result> result;
    };
    struct Event {
        std::function<void()> action;
        size_t bytes;
    };
    AMTContext& ctx;
    Callback callback;
    const bool asynchronous;
    std::map<int, std::unique_ptr<AdtsParser>> parsers;
    std::deque<Event> events;
    size_t bufferedBytes = 0;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<std::shared_ptr<Job>> jobs;
    bool stopping = false;
    std::thread worker;

    Impl(AMTContext& ctx, Callback callback, bool asynchronous)
        : ctx(ctx), callback(std::move(callback)), asynchronous(asynchronous) {
        if (asynchronous) worker = std::thread([this]() { run(); });
    }
    ~Impl() {
        // ワーカーは主スレッドの反映を待たないので、例外中も確実に回収できる。
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            jobs.clear();
        }
        available.notify_one();
        if (worker.joinable()) worker.join();
    }
    Result decode(const Job& job) {
        auto& parser = parsers[job.audioIdx];
        if (!parser) parser.reset(new AdtsParser(ctx));
        Result result;
        parser->setDiagnostics(&result.diagnostics);
        PESPacket packet(MemoryChunk(const_cast<uint8_t*>(job.packet.data()), job.packet.size()));
        packet.parse();
        std::vector<AudioFrameData> frames;
        try {
            parser->inputFrame(packet.paylod(), frames, packet.has_PTS() ? packet.PTS : -1);
            result.frames.reserve(frames.size());
            for (const auto& info : frames) {
                Frame frame;
                frame.info = info;
                frame.coded.assign(info.codedData, info.codedData + info.codedDataSize);
                if (info.decodedDataSize > 0) {
                    frame.decoded.resize(info.decodedDataSize / sizeof(int16_t));
                    memcpy(frame.decoded.data(), info.decodedData, info.decodedDataSize);
                }
                result.frames.emplace_back(std::move(frame));
            }
        } catch (...) {
            parser->setDiagnostics(nullptr);
            throw;
        }
        parser->setDiagnostics(nullptr);
        return result;
    }
    void run() {
        std::exception_ptr failure;
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                available.wait(lock, [this]() { return stopping || !jobs.empty(); });
                if (stopping) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            try {
                if (failure) std::rethrow_exception(failure);
                job->result.set_value(decode(*job));
            } catch (...) {
                failure = std::current_exception();
                job->result.set_exception(failure);
            }
        }
    }
    void commit(const Job& job, Result& result) {
        for (int i = 0; i < result.diagnostics.errors; i++) ctx.incrementCounter(AMT_ERR_DECODE_AUDIO);
        for (const auto& warning : result.diagnostics.warnings) ctx.warn(warning);
        if (result.frames.empty()) return;
        std::vector<AudioFrameData> frames;
        frames.reserve(result.frames.size());
        for (auto& frame : result.frames) {
            frame.info.codedData = frame.coded.data();
            frame.info.decodedData = frame.decoded.data();
            frames.push_back(frame.info);
        }
        PESPacket packet(MemoryChunk(const_cast<uint8_t*>(job.packet.data()), job.packet.size()));
        packet.parse();
        callback(job.audioIdx, job.clock, frames, packet, job.component);
    }
    void pop() {
        auto event = std::move(events.front());
        events.pop_front();
        bufferedBytes -= event.bytes;
        event.action();
    }
    void reserve(size_t bytes) {
        // 件数とデコード後の最大サイズ概算を制限する。単一の巨大PESは従来と同じく一件分だけ許容。
        constexpr size_t MAX_BYTES = 64 * 1024 * 1024;
        constexpr size_t MAX_EVENTS = 64;
        while (!events.empty() && (events.size() >= MAX_EVENTS || bytes > MAX_BYTES || bufferedBytes > MAX_BYTES - bytes)) pop();
    }
};

AudioPesPipeline::AudioPesPipeline(AMTContext& ctx, Callback callback, bool asynchronous)
    : impl_(new Impl(ctx, std::move(callback), asynchronous)) {}
AudioPesPipeline::~AudioPesPipeline() = default;

void AudioPesPipeline::submit(int audioIdx, int64_t clock, PESPacket packet, const AudioComponentInfo& component) {
    if (clock == -1) {
        drain();
        impl_->ctx.error(_T("Audio PES Packet にクロック情報がありません"));
        return;
    }
    // ADTSの最短ヘッダーとSBRを含むPCM最大長から、PESを跨いだ残りも見積もる。
    constexpr size_t MAX_REMAINDER_BYTES = 8192;
    constexpr size_t MAX_DECODED_EXPANSION = 1200;
    const size_t estimatedBytes = packet.length > (SIZE_MAX / MAX_DECODED_EXPANSION - MAX_REMAINDER_BYTES)
        ? SIZE_MAX : (packet.length + MAX_REMAINDER_BYTES) * MAX_DECODED_EXPANSION;
    impl_->reserve(estimatedBytes);
    auto job = std::make_shared<Impl::Job>();
    job->audioIdx = audioIdx;
    job->clock = clock;
    job->component = component;
    job->packet.assign(packet.data, packet.data + packet.length);
    if (!impl_->asynchronous) {
        auto result = impl_->decode(*job);
        impl_->commit(*job, result);
        return;
    }
    auto future = std::make_shared<std::future<Impl::Result>>(job->result.get_future());
    impl_->events.push_back({ [this, job, future]() {
        auto result = future->get();
        impl_->commit(*job, result);
    }, estimatedBytes });
    impl_->bufferedBytes += estimatedBytes;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->jobs.push_back(std::move(job));
    }
    impl_->available.notify_one();
}

void AudioPesPipeline::defer(std::function<void()> action, size_t bytes) {
    if (!impl_->asynchronous || impl_->events.empty()) {
        action();
        return;
    }
    impl_->reserve(bytes);
    impl_->events.push_back({ std::move(action), bytes });
    impl_->bufferedBytes += bytes;
}
bool AudioPesPipeline::hasPending() const {
    return !impl_->events.empty();
}
void AudioPesPipeline::drain() {
    while (!impl_->events.empty()) impl_->pop();
}


namespace {
std::vector<uint8_t> MakePipelineTestPes(const std::vector<uint8_t>& payload, int64_t pts) {
    std::vector<uint8_t> data = { 0, 0, 1, 0xc0, 0, 0, 0x80, 0x80, 5, 0x21, 0, 1, 0, 1 };
    data.insert(data.end(), payload.begin(), payload.end());
    PESPacket packet(MemoryChunk(data.data(), data.size()));
    packet.parse();
    packet.changeTimestamp(pts, pts);
    packet.writePacketLength();
    return data;
}

}

extern "C" AMATSUKAZE_API int VerifyAudioPesPipelineForTest(const AudioPesForTest* packets, size_t packetCount) {
    if (packets == nullptr || packetCount == 0) return -5;
    for (size_t i = 0; i < packetCount; i++) {
        if (packets[i].data == nullptr || packets[i].length == 0) return -5;
    }
    try {
        auto run = [&](bool asynchronous) {
            AMTContext ctx;
            ctx.setTimePrefix(false);
            std::vector<int64_t> metadata;
            std::vector<uint8_t> bytes;
            AudioPesPipeline pipeline(ctx, [&](int index, int64_t clock, const std::vector<AudioFrameData>& frames, PESPacket packet, const AudioComponentInfo& component) {
                const int sequence = static_cast<int>(clock / 270000);
                if (!component.valid || component.language != sequence + 1
                    || component.mainComponent != ((sequence % 2) == 0)) {
                    throw std::runtime_error("音声コンポーネント情報の保持結果が一致しません");
                }
                metadata.insert(metadata.end(), { index, clock, packet.PTS, (int64_t)frames.size() });
                for (const auto& frame : frames) {
                    metadata.insert(metadata.end(), { frame.PTS, frame.numSamples, frame.format.channels,
                        frame.format.sampleRate, frame.numDecodedSamples, frame.codedDataSize, frame.decodedDataSize });
                    bytes.insert(bytes.end(), frame.codedData, frame.codedData + frame.codedDataSize);
                    const auto* decoded = reinterpret_cast<const uint8_t*>(frame.decodedData);
                    if (frame.decodedDataSize > 0) bytes.insert(bytes.end(), decoded, decoded + frame.decodedDataSize);
                }
            }, asynchronous);
            // 入力を直後に上書きし、複数トラック・PES跨ぎ・形式変更・多数イベントを混在させる。
            for (int i = 0; i < 180; i++) {
                const auto& input = packets[i % packetCount];
                std::vector<uint8_t> payload(input.data, input.data + input.length);
                auto data = MakePipelineTestPes(payload, input.PTS + i * 4000);
                PESPacket packet(MemoryChunk(data.data(), data.size()));
                packet.parse();
                AudioComponentInfo component;
                component.valid = true;
                component.language = i + 1;
                component.mainComponent = (i % 2) == 0;
                pipeline.submit((i / 5) % 2, i * 270000, packet, component);
                component = {}; // 呼出元が更新してもジョブ内の情報を維持する
                std::fill(data.begin(), data.end(), 0);
                const int marker = i;
                pipeline.defer([&, marker]() { metadata.insert(metadata.end(), { -2, marker }); }, 1024);
                if (i == 77) pipeline.drain();
            }
            pipeline.drain();
            return std::make_pair(metadata, bytes);
        };
        if (run(false) != run(true)) return -1;

        AMTContext ctx;
        int committed = 0;
        auto data = MakePipelineTestPes(std::vector<uint8_t>(packets[0].data, packets[0].data + packets[0].length), packets[0].PTS);
        PESPacket packet(MemoryChunk(data.data(), data.size()));
        packet.parse();
        {
            AudioPesPipeline pipeline(ctx, [&](int, int64_t, const std::vector<AudioFrameData>&, PESPacket, const AudioComponentInfo&) { committed++; });
            pipeline.submit(0, 0, packet);
            pipeline.defer([&]() { committed++; });
        }
        if (committed != 0) return -2;
        try {
            AudioPesPipeline pipeline(ctx, [&](int, int64_t, const std::vector<AudioFrameData>&, PESPacket, const AudioComponentInfo&) {
                throw std::runtime_error("反映時の例外");
            });
            for (int i = 0; i < 4; i++) pipeline.submit(0, 0, packet);
            pipeline.defer([&]() { committed++; });
            pipeline.drain();
            return -3;
        } catch (const std::runtime_error&) {
        }
        return committed == 0 ? 0 : -4;
    } catch (...) {
        return -6;
    }
}

VideoFrameParser::VideoFrameParser(AMTContext&ctx)
    : AMTObject(ctx)
    , PesParser()
    , videoStreamFormat(VS_MPEG2)
    , videoFormat()
    , mpeg2parser(ctx)
    , h264parser(ctx)
    , hevcparser(ctx)
    , parser(&mpeg2parser) {}

void VideoFrameParser::setStreamFormat(VIDEO_STREAM_FORMAT streamFormat) {
    if (videoStreamFormat != streamFormat) {
        switch (streamFormat) {
        case VS_MPEG2:
            parser = &mpeg2parser;
            break;
        case VS_H264:
            parser = &h264parser;
            break;
        case VS_H265:
            parser = &hevcparser;
            break;
        default:
            break;
        }
        reset();
        videoStreamFormat = streamFormat;
    }
}

VIDEO_STREAM_FORMAT VideoFrameParser::getStreamFormat() { return videoStreamFormat; }

void VideoFrameParser::reset() {
    videoFormat = VideoFormat();
    parser->reset();
}

extern "C" int ParseVideoAccessUnitsForTest(int streamFormat,
    const VideoAccessUnitForTest* units, size_t unitCount, int* unitResults,
    VideoFrameInfo* output, size_t outputCapacity, size_t* outputCount) {
    if (outputCount == nullptr || (unitCount != 0 && units == nullptr)) {
        return VIDEO_PARSER_FOR_TEST_INVALID_ARGUMENT;
    }
    *outputCount = 0;
    try {
        AMTContext ctx;
        MPEG2VideoParser mpeg2parser(ctx);
        H264VideoParser h264parser(ctx);
        HEVCVideoParser hevcparser(ctx);
        IVideoParser* parser = nullptr;
        switch (streamFormat) {
        case VS_MPEG2: parser = &mpeg2parser; break;
        case VS_H264: parser = &h264parser; break;
        case VS_H265: parser = &hevcparser; break;
        default: return VIDEO_PARSER_FOR_TEST_INVALID_ARGUMENT;
        }
        // VideoFrameParser::setStreamFormatと同じく、使用前にリセットする
        parser->reset();
        std::vector<VideoFrameInfo> frames;
        std::vector<VideoFrameInfo> unitFrames;
        for (size_t i = 0; i < unitCount; i++) {
            if (units[i].data == nullptr) {
                return VIDEO_PARSER_FOR_TEST_INVALID_ARGUMENT;
            }
            const bool ok = parser->inputFrame(MemoryChunk(const_cast<uint8_t*>(units[i].data), units[i].length),
                unitFrames, units[i].PTS, units[i].DTS);
            if (unitResults != nullptr) {
                unitResults[i] = ok ? 1 : 0;
            }
            frames.insert(frames.end(), unitFrames.begin(), unitFrames.end());
        }
        *outputCount = frames.size();
        if (output == nullptr) {
            return outputCapacity == 0 ? VIDEO_PARSER_FOR_TEST_SUCCESS : VIDEO_PARSER_FOR_TEST_INVALID_ARGUMENT;
        }
        if (outputCapacity < frames.size()) {
            return VIDEO_PARSER_FOR_TEST_BUFFER_TOO_SMALL;
        }
        std::copy(frames.begin(), frames.end(), output);
        return VIDEO_PARSER_FOR_TEST_SUCCESS;
    } catch (...) {
        *outputCount = 0;
        return VIDEO_PARSER_FOR_TEST_FAILED;
    }
}

/* virtual */ void VideoFrameParser::onPesPacket(int64_t clock, PESPacket packet) {
    if (!packet.has_PTS()) {
        ctx.error(_T("Video PES Packet に PTS がありません"));
        return;
    }

    int64_t PTS = packet.has_PTS() ? packet.PTS : -1;
    int64_t DTS = packet.has_DTS() ? packet.DTS : PTS;
    MemoryChunk payload = packet.paylod();

    if (!parser->inputFrame(payload, frameInfo, PTS, DTS)) {
        ctx.errorF(_T("フレーム情報の取得に失敗 PTS=%lld"), PTS);
        return;
    }

    if (frameInfo.size() > 0) {
        const VideoFrameInfo& frame = frameInfo[0];

        if (frame.format.isEmpty()) {
            // フォーマットがわからないとデコードできないので流さない
            return;
        }

        if (frame.format != videoFormat) {
            // フォーマットが変わった
            videoFormat = frame.format;
            onVideoFormatChanged(frame.format);
        }

        onVideoPesPacket(clock, frameInfo, packet);
    }
}
AudioFrameParser::AudioFrameParser(AMTContext&ctx)
    : AMTObject(ctx)
    , PesParser()
    , adtsParser(ctx) {}

/* virtual */ void AudioFrameParser::onPesPacket(int64_t clock, PESPacket packet) {
    if (clock == -1) {
        ctx.error(_T("Audio PES Packet にクロック情報がありません"));
        return;
    }

    int64_t PTS = packet.has_PTS() ? packet.PTS : -1;
    int64_t DTS = packet.has_DTS() ? packet.DTS : PTS;
    MemoryChunk payload = packet.paylod();

    adtsParser.inputFrame(payload, frameData, PTS);

    if (frameData.size() > 0) {
        const AudioFrameData& frame = frameData[0];

        if (frame.format != format) {
            // フォーマットが変わった
            format = frame.format;
            onAudioFormatChanged(frame.format);
        }

        onAudioPesPacket(clock, frameData, packet);
    }
}
CaptionParser::CaptionParser(AMTContext&ctx)
    : AMTObject(ctx)
    , PesParser()
    , fomatter(*this) {}

/* virtual */ void CaptionParser::onPesPacket(int64_t clock, PESPacket packet) {
    int64_t PTS = packet.has_PTS() ? packet.PTS : -1;
    int64_t SysClock = clock / 300;

    // 基本的にPTSを処理可能な受信機だが、
    // PTSが正しくない場合があるので、その場合は
    // PTSを処理不能な受信機をエミューレーション
    // PTSとSysClockとの差は
    //  - ARIBによれば受信から表示まで少なくとも0.5秒以上空けるようにとある
    //  - 複数のTSを確認したところだいたい0.75～0.80秒くらいだった
    // ので、0.5秒～1.5秒を正しいと判定、それ以外は0.8秒先を表示時刻とする
    auto Td = PTS - SysClock;
    if (Td < 0.5 * MPEG_CLOCK_HZ || Td > 1.5 * MPEG_CLOCK_HZ) {
        PTS = SysClock + (int)(0.8 * MPEG_CLOCK_HZ);
        //ctx.info("字幕PTSを修正 %d", PTS);
    }

    //int64_t DTS = packet.has_DTS() ? packet.DTS : PTS;
    MemoryChunk payload = packet.paylod();
    // 管理データやデコード結果なしのPESも、同じ補正済みPTSで通知する。
    onRawCaptionPesPacket(PTS, payload);

    captions.clear();

    DWORD ret = AddPESPacketCP(payload.data, (DWORD)payload.length);

    if (ret >= CP_NO_ERR_CAPTION_1 && ret <= CP_NO_ERR_CAPTION_8) {
        int ucLangTag = ret - CP_NO_ERR_CAPTION_1;
        // 字幕文データ
        CAPTION_DATA_DLL* capList = nullptr;
        DWORD capCount = 0;
        DRCS_PATTERN_DLL* drcsList = nullptr;
        DWORD drcsCount = 0;
        if (GetCaptionDataCPW(ucLangTag, &capList, &capCount) != TRUE) {
            capCount = 0;
        } else {
            // DRCS図形データ(あれば)
            if (GetDRCSPatternCP(ucLangTag, &drcsList, &drcsCount) != TRUE) {
                drcsCount = 0;
            }
            for (DWORD i = 0; i < capCount; i++) {
                captions.emplace_back(
                    fomatter.ProcessCaption(PTS, ucLangTag,
                        capList + i, capCount - i, drcsList, drcsCount));
            }
        }
    } else if (ret == CP_CHANGE_VERSION) {
        // 字幕管理データ
        // 字幕のフォーマット変更は考慮しないので今のところ見る必要なし
    } else if (ret == CP_NO_ERR_TAG_INFO) {
        //
    } else if (ret != TRUE && ret != CP_ERR_NEED_NEXT_PACKET &&
        (ret < CP_NO_ERR_CAPTION_1 || CP_NO_ERR_CAPTION_8 < ret)) {
        // エラーパケット
    }

    if (captions.size() > 0) {
        onCaptionPesPacket(clock, captions, packet);
    }
}
CaptionParser::SpCaptionFormatter::SpCaptionFormatter(CaptionParser& this_)
    : CaptionDLLParser(this_.ctx), this_(this_) {}
/* virtual */ DRCSOutInfo CaptionParser::SpCaptionFormatter::getDRCSOutPath(int64_t PTS, const std::string& md5) {
    return this_.getDRCSOutPath(PTS, md5);
}
TsPacketBuffer::TsPacketBuffer(AMTContext& ctx)
    : TsPacketParser(ctx)
    , handler(NULL)
    , numBefferedPackets_(0)
    , numMaxPackets(0)
    , buffering(false) {}

void TsPacketBuffer::setHandler(TsPacketHandler* handler) {
    this->handler = handler;
}

int TsPacketBuffer::numBefferedPackets() {
    return numBefferedPackets_;
}

void TsPacketBuffer::clearBuffer() {
    buffer.clear();
    numBefferedPackets_ = 0;
}

void TsPacketBuffer::setEnableBuffering(bool enable) {
    buffering = enable;
    if (!buffering) {
        clearBuffer();
    }
}

void TsPacketBuffer::setNumBufferingPackets(int numPackets) {
    numMaxPackets = numPackets;
}

void TsPacketBuffer::backAndInput() {
    if (handler != NULL) {
        for (int i = 0; i < (int)buffer.size(); i += TS_PACKET_LENGTH) {
            TsPacket packet(buffer.ptr() + i);
            if (packet.parse() && packet.check()) {
                handler->onTsPacket(-1, packet);
            }
        }
    }
}

/* virtual */ void TsPacketBuffer::onTsPacket(TsPacket packet) {
    if (buffering) {
        if (numBefferedPackets_ >= numMaxPackets) {
            buffer.trimHead((numMaxPackets - numBefferedPackets_ + 1) * TS_PACKET_LENGTH);
            numBefferedPackets_ = numMaxPackets - 1;
        }
        buffer.add(MemoryChunk(packet.data, TS_PACKET_LENGTH));
        numBefferedPackets_++;
    }
    if (handler != NULL) {
        handler->onTsPacket(-1, packet);
    }
}
TsSystemClock::TsSystemClock(AMTContext& ctx)
    : AMTObject(ctx)
    , PcrPid(-1)
    , numPcrReceived(0)
    , numTotakPacketsReveived(0)
    , pcrInfo() {}

void TsSystemClock::setPcrPid(int PcrPid) {
    this->PcrPid = PcrPid;
}

// 十分な数のPCRを受信したか
bool TsSystemClock::pcrReceived() {
    return numPcrReceived >= 2;
}

// 現在入力されたパケットを基準にしてrelativeだけ後のパケットの入力時刻を返す
int64_t TsSystemClock::getClock(int relative) {
    if (!pcrReceived()) {
        return -1;
    }
    int index = numTotakPacketsReveived + relative - 1;
    int64_t clockDiff = pcrInfo[1].clock - pcrInfo[0].clock;
    int64_t indexDiff = pcrInfo[1].packetIndex - pcrInfo[0].packetIndex;
    return clockDiff * (index - pcrInfo[1].packetIndex) / indexDiff + pcrInfo[1].clock;
}

// TSストリームを最初から読み直すときに呼び出す
void TsSystemClock::backTs() {
    numTotakPacketsReveived = 0;
}

// TSストリームの全データを入れること
void TsSystemClock::inputTsPacket(TsPacket packet) {
    if (packet.PID() == PcrPid) {
        if (packet.has_adaptation_field()) {
            MemoryChunk data = packet.adapdation_field();
            AdapdationField af(data.data, (int)data.length);
            if (af.parse() && af.check()) {
                // 従来のコメントに、「ARIB TR-14 8.2.3に送出規定があって、TSパケットの連続性、PCR連続性が切れる場合に送られる」とある
                // しかし、該当の記述はDIT(PID=0x001e)に関するもので、必ずしもPCRのPIDに対する記述ではないように読める
                // 実際、PCRと映像のPIDが一致する場合に、ここでPCRのリセットをしてしまうと、実際にはPCRが連続にもかかわらず、
                // "Audio/Video PES Packet にクロック情報がありません"となってdropしてしまう
                // そこでaf.discontinuity_indicator()による処理は無効化する
                //if (af.discontinuity_indicator()) {
                //    // PCRが連続でないのでリセット
                //    numPcrReceived = 0;
                //}
                if (pcrInfo[1].packetIndex < numTotakPacketsReveived) {
                    std::swap(pcrInfo[0], pcrInfo[1]);
                    if (af.PCR_flag()) {
                        pcrInfo[1].clock = af.program_clock_reference;
                        pcrInfo[1].packetIndex = numTotakPacketsReveived;
                        numPcrReceived++;
                    }

                    // テスト用
                    //if (pcrReceived()) {
                    //	PRINTF("PCR: %f Mbps\n", currentBitrate() / (1024 * 1024));
                    //}
                }
            }
        }
    }
    numTotakPacketsReveived++;
}

double TsSystemClock::currentBitrate() {
    int clockDiff = int(pcrInfo[1].clock - pcrInfo[0].clock);
    int indexDiff = int(pcrInfo[1].packetIndex - pcrInfo[0].packetIndex);
    return (double)(indexDiff * TS_PACKET_LENGTH * 8) / clockDiff * 27000000;
}

TsSplitter::TsSplitter(AMTContext& ctx, bool enableVideo, bool enableAudio, bool enableCaption)
    : AMTObject(ctx)
    , initPhase(PMT_WAITING)
    , tsPacketParser(ctx)
    , tsSystemClock(ctx)
    , tsPacketHandler(*this)
    , pcrDetectionHandler(*this)
    , tsPacketSelector(ctx)
    , videoParser(ctx, *this)
    , captionParser(ctx, *this)
    , enableVideo(enableVideo)
    , enableAudio(enableAudio)
    , enableCaption(enableCaption)
    , warnedInvalidAudioIndex(false)
    , numTotalPackets(0)
    , numScramblePackets(0) {
    tsPacketParser.setHandler(&tsPacketHandler);
    tsPacketParser.setNumBufferingPackets(50 * 1024); // 9.6MB
    tsPacketSelector.setHandler(this);
    reset();
}

void TsSplitter::reset() {
    initPhase = PMT_WAITING;
    preferedServiceId = -1;
    selectedServiceId = -1;
    warnedInvalidAudioIndex = false;
    tsPacketParser.setEnableBuffering(true);
}

// 0以下で指定無効
void TsSplitter::setServiceId(int sid) {
    preferedServiceId = sid;
}

int TsSplitter::getActualServiceId() {
    return selectedServiceId;
}

void TsSplitter::inputTsData(MemoryChunk data) {
    tsPacketParser.inputTS(data);
}
void TsSplitter::flush() {
    tsPacketParser.flush();
}

int64_t TsSplitter::getNumTotalPackets() const {
    return numTotalPackets;
}

int64_t TsSplitter::getNumScramblePackets() const {
    return numScramblePackets;
}
TsSplitter::SpTsPacketHandler::SpTsPacketHandler(TsSplitter& this_)
    : this_(this_) {}

/* virtual */ void TsSplitter::SpTsPacketHandler::onTsPacket(int64_t clock, TsPacket packet) {
    this_.tsSystemClock.inputTsPacket(packet);

    int64_t packetClock = this_.tsSystemClock.getClock(0);
    this_.tsPacketSelector.inputTsPacket(packetClock, packet);
}
TsSplitter::PcrDetectionHandler::PcrDetectionHandler(TsSplitter& this_)
    : this_(this_) {}

/* virtual */ void TsSplitter::PcrDetectionHandler::onTsPacket(int64_t clock, TsPacket packet) {
    this_.tsSystemClock.inputTsPacket(packet);
    if (this_.tsSystemClock.pcrReceived()) {
        this_.ctx.debug(_T("必要な情報は取得したのでTSを最初から読み直します"));
        this_.initPhase = INIT_FINISHED;
        // ハンドラを戻して最初から読み直す
        this_.tsPacketParser.setHandler(&this_.tsPacketHandler);
        this_.tsPacketSelector.resetParser();
        this_.tsSystemClock.backTs();

        int64_t startClock = this_.tsSystemClock.getClock(0);
        this_.ctx.infoF(_T("開始Clock: %lld"), startClock);
        this_.tsPacketSelector.setStartClock(startClock);

        this_.tsPacketParser.backAndInput();
        // もう必要ないのでバッファリングはOFF
        this_.tsPacketParser.setEnableBuffering(false);
    }
}
TsSplitter::SpVideoFrameParser::SpVideoFrameParser(AMTContext&ctx, TsSplitter& this_)
    : VideoFrameParser(ctx), this_(this_) {}
/* virtual */ void TsSplitter::SpVideoFrameParser::onVideoPesPacket(int64_t clock, const std::vector<VideoFrameInfo>& frames, PESPacket packet) {
    if (clock == -1) {
        ctx.error(_T("Video PES Packet にクロック情報がありません"));
        return;
    }
    this_.onVideoPesPacket(clock, frames, packet);
}

/* virtual */ void TsSplitter::SpVideoFrameParser::onVideoFormatChanged(VideoFormat fmt) {
    this_.onVideoFormatChanged(fmt);
}
TsSplitter::SpAudioFrameParser::SpAudioFrameParser(AMTContext&ctx, TsSplitter& this_, int audioIdx)
    : AudioFrameParser(ctx), this_(this_), audioIdx(audioIdx) {}
void TsSplitter::SpAudioFrameParser::onPesPacketStarted() {
    component = this_.tsPacketSelector.getAudioComponentInfo(audioIdx);
}
/* virtual */ void TsSplitter::SpAudioFrameParser::onPesPacket(int64_t clock, PESPacket packet) {
    if (!this_.onAudioPesReady(audioIdx, clock, packet, component)) AudioFrameParser::onPesPacket(clock, packet);
}
/* virtual */ void TsSplitter::SpAudioFrameParser::onAudioPesPacket(int64_t clock, const std::vector<AudioFrameData>& frames, PESPacket packet) {
    this_.onAudioPesPacket(audioIdx, clock, frames, packet, component);
}

/* virtual */ void TsSplitter::SpAudioFrameParser::onAudioFormatChanged(AudioFormat fmt) {
    this_.onAudioFormatChanged(audioIdx, fmt);
}
TsSplitter::SpCaptionParser::SpCaptionParser(AMTContext&ctx, TsSplitter& this_)
    : CaptionParser(ctx), this_(this_) {}
/* virtual */ void TsSplitter::SpCaptionParser::onRawCaptionPesPacket(int64_t PTS, MemoryChunk payload) {
    this_.onRawCaptionPesPacket(PTS, payload);
}
/* virtual */ void TsSplitter::SpCaptionParser::onCaptionPesPacket(int64_t clock, std::vector<CaptionItem>& captions, PESPacket packet) {
    this_.onCaptionPesPacket(clock, captions, packet);
}

/* virtual */ DRCSOutInfo TsSplitter::SpCaptionParser::getDRCSOutPath(int64_t PTS, const std::string& md5) {
    return this_.getDRCSOutPath(PTS, md5);
}

// サービスを設定する場合はサービスのpids上でのインデックス
// なにもしない場合は負の値の返す
/* virtual */ int TsSplitter::onPidSelect(int TSID, const std::vector<int>& pids) {
    ctx.info(_T("[PAT更新]"));
    for (int i = 0; i < int(pids.size()); i++) {
        if (preferedServiceId == pids[i]) {
            selectedServiceId = pids[i];
            ctx.infoF(_T("サービス %d を選択"), selectedServiceId);
            return i;
        }
    }
    if (preferedServiceId > 0) {
        // サービス指定があるのに該当サービスがなかったらエラーとする
        StringBuilder sb;
        sb.append("サービスID: ");
        for (int i = 0; i < (int)pids.size(); i++) {
            sb.append("%s%d", (i > 0) ? ", " : "", pids[i]);
        }
        sb.append(" 指定サービスID: %d", preferedServiceId);
        ctx.error(_T("指定されたサービスがありません"));
        ctx.error(char_to_tstring(sb.str()));
        //THROW(InvalidOperationException, "failed to select service");
    }
    selectedServiceId = pids[0];
    ctx.infoF(_T("サービス %d を選択（指定がありませんでした）"), selectedServiceId);
    return 0;
}

/* virtual */ void TsSplitter::onPmtUpdated(int PcrPid) {
    if (initPhase == PMT_WAITING) {
        initPhase = PCR_WAITING;
        // PCRハンドラに置き換えてTSを最初から読み直す
        tsPacketParser.setHandler(&pcrDetectionHandler);
        tsSystemClock.setPcrPid(PcrPid);
        tsPacketSelector.resetParser();
        tsSystemClock.backTs();
        tsPacketParser.backAndInput();
    }
}

// TsPacketSelectorでPID Tableが変更された時変更後の情報が送られる
/* virtual */ void TsSplitter::onPidTableChanged(const PMTESInfo video, const std::vector<PMTESInfo>& audio, const PMTESInfo caption) {
    if (enableVideo || enableAudio) {
        // 映像ストリーム形式をセット
        switch (video.stype) {
        case 0x02: // MPEG2-VIDEO
            videoParser.setStreamFormat(VS_MPEG2);
            break;
        case 0x1B: // H.264/AVC
            videoParser.setStreamFormat(VS_H264);
            break;
        case 0x24: // H.265/HEVC
            videoParser.setStreamFormat(VS_H265);
            break;
        }

        // 必要な数だけ音声パーサ作る
        size_t numAudios = audio.size();
        while (audioParsers.size() < numAudios) {
            int audioIdx = int(audioParsers.size());
            audioParsers.push_back(new SpAudioFrameParser(ctx, *this, audioIdx));
            ctx.infoF(_T("音声パーサ %d を追加"), audioIdx);
        }
    }
}

bool TsSplitter::checkScramble(TsPacket packet) {
    numTotalPackets++;
    if (packet.transport_scrambling_control()) {
        numScramblePackets++;
        return false;
    }
    return true;
}

/* virtual */ void TsSplitter::onVideoPacket(int64_t clock, TsPacket packet) {
    if (enableVideo && checkScramble(packet)) videoParser.onTsPacket(clock, packet);
}

/* virtual */ void TsSplitter::onAudioPacket(int64_t clock, TsPacket packet, int audioIdx) {
    if (enableAudio && checkScramble(packet)) {
        if (audioIdx < 0 || audioIdx >= (int)audioParsers.size() || audioParsers[audioIdx] == nullptr) {
            if (!warnedInvalidAudioIndex) {
                ctx.warnF(_T("音声PIDハンドラのインデックスが範囲外です。該当パケットを無視します: index=%d parsers=%d"),
                    audioIdx, (int)audioParsers.size());
                warnedInvalidAudioIndex = true;
            }
            return;
        }
        audioParsers[audioIdx]->onTsPacket(clock, packet);
    }
}

/* virtual */ void TsSplitter::onCaptionPacket(int64_t clock, TsPacket packet) {
    if (enableCaption && checkScramble(packet)) captionParser.onTsPacket(clock, packet);
}
