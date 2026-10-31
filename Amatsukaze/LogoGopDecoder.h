#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace logo {

// 並列復号だけで処理できない入力を、解析全体の再試行対象として区別する。
class LogoGopDecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// パケットの復号順と表示順を分離し、timestampを変更せずに担当範囲を追跡する。
struct LogoGopDecodeSettings {
    int decoders = 4;
    int threadsPerDecoder = 1;
    int gopsPerJob = 8;
    int outputQueueDepth = 100;
    size_t frameBufferBytes = size_t(2048) << 20;
};

struct LogoGopDecodeReport {
    int64_t jobs = 0;
    int64_t packets = 0;
    int64_t frames = 0;
    int64_t prerollPackets = 0;
};

namespace gopdecode {

inline void Check(int result, const char* operation) {
    if (result < 0) {
        if (result == AVERROR(ENOMEM)) throw std::bad_alloc();
        char detail[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(result, detail, sizeof(detail));
        throw LogoGopDecodeError(std::string(operation) + ": " + detail);
    }
}

struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct CodecDeleter { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
using Packet = std::shared_ptr<AVPacket>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Codec = std::unique_ptr<AVCodecContext, CodecDeleter>;

struct QueueSignals {
    std::mutex mutex;
    std::condition_variable data;
    std::condition_variable room;
};

struct State {
    std::mutex mutex;
    std::atomic<bool> stopped{false};
    std::vector<std::weak_ptr<QueueSignals>> queues;
    std::exception_ptr error;

    void add(const std::shared_ptr<QueueSignals>& queue) {
        std::lock_guard<std::mutex> lock(mutex);
        queues.erase(std::remove_if(queues.begin(), queues.end(), [](const auto& q) { return q.expired(); }), queues.end());
        queues.push_back(queue);
    }

    void stop(std::exception_ptr failure = nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        stopped.store(true, std::memory_order_release);
        if (failure && !error) error = failure;
        for (auto& weak : queues) {
            if (auto queue = weak.lock()) {
                // 待機開始との競合で停止通知を失わないよう、各キューのロックを取る。
                std::lock_guard<std::mutex> queueLock(queue->mutex);
                queue->data.notify_all();
                queue->room.notify_all();
            }
        }
    }
};

// 通常の通知は対応するキューだけへ送り、停止時だけ全段を起こす。
template<typename T> class Queue {
    State& state;
    std::shared_ptr<QueueSignals> signals = std::make_shared<QueueSignals>();
    const size_t capacity;
    std::deque<T> values;
    bool closed = false;
public:
    Queue(State& state, size_t capacity) : state(state), capacity(capacity) { state.add(signals); }
    bool isStopped() const { return state.stopped.load(std::memory_order_acquire); }
    bool push(T value) {
        std::unique_lock<std::mutex> lock(signals->mutex);
        signals->room.wait(lock, [&]() { return state.stopped || closed || values.size() < capacity; });
        if (state.stopped || closed) return false;
        values.push_back(std::move(value));
        lock.unlock();
        signals->data.notify_one();
        return true;
    }
    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(signals->mutex);
        signals->data.wait(lock, [&]() { return state.stopped || closed || !values.empty(); });
        if (state.stopped || values.empty()) return false;
        value = std::move(values.front());
        values.pop_front();
        lock.unlock();
        signals->room.notify_one();
        return true;
    }
    void close() {
        {
            std::lock_guard<std::mutex> lock(signals->mutex);
            closed = true;
        }
        signals->data.notify_all();
        signals->room.notify_all();
    }
};

struct Gop {
    int64_t firstPacket = 0;
    std::vector<Packet> packets;
    std::shared_ptr<std::vector<uint8_t>> sequence;
};

struct Output {
    Frame frame;
    int64_t packetNumber = 0;
    int64_t sourcePosition = -1;
};

struct Job {
    int64_t number = 0;
    int64_t inputBegin = 0;
    int64_t outputBegin = 0;
    int64_t outputEnd = 0;
    std::vector<Packet> packets;
    std::shared_ptr<std::vector<uint8_t>> sequence;
    Queue<Output> output;
    Job(State& state, int depth) : output(state, depth) {}
};

struct PacketHeader {
    bool startsGop = false;
    bool fieldPicture = false;
    bool completeFieldPair = false;
    bool matrixUpdate = false;
    int sequenceOffset = -1;
    int pictureOffset = -1;
};

// 通常フレームの走査を増やさず、フィールド入力だけ上下ペア全体を検証する。
inline bool InspectFieldPair(const AVPacket& packet, PacketHeader& header) {
    if (header.pictureOffset < 0) return false;
    int pictures = 0;
    int temporalReference = -1;
    int structures[2] = {};
    bool slices[2] = {};
    for (int i = header.pictureOffset; i + 3 < packet.size; i++) {
        if (packet.data[i] || packet.data[i + 1] || packet.data[i + 2] != 1) continue;
        const int code = packet.data[i + 3];
        if (code == 0) {
            if (++pictures > 2 || i + 5 >= packet.size) return false;
            const int reference = (int(packet.data[i + 4]) << 2) | (packet.data[i + 5] >> 6);
            if (temporalReference < 0) temporalReference = reference;
            else if (reference != temporalReference) return false;
        } else if (code >= 1 && code <= 0xaf) {
            if (pictures == 0 || structures[pictures - 1] == 0) return false;
            slices[pictures - 1] = true;
        } else if (code == 0xb3 || code == 0xb8) {
            return false;
        } else if (code == 0xb5) {
            if (i + 4 >= packet.size) return false;
            const int extension = packet.data[i + 4] >> 4;
            // ピクチャー開始後の行列更新は、後続ジョブへ状態を引き継げない。
            if (extension == 3) header.matrixUpdate = true;
            if (extension == 8) {
                if (pictures == 0 || i + 6 >= packet.size || structures[pictures - 1] != 0) return false;
                const int structure = packet.data[i + 6] & 3;
                if (structure != 1 && structure != 2) return false;
                structures[pictures - 1] = structure;
            }
        }
    }
    return pictures == 2 && structures[0] != structures[1] && slices[0] && slices[1];
}

inline PacketHeader Inspect(const AVPacket& packet) {
    PacketHeader out;
    bool hasGop = false;
    for (int i = 0; i + 5 < packet.size; i++) {
        if (packet.data[i] || packet.data[i + 1] || packet.data[i + 2] != 1) continue;
        const int code = packet.data[i + 3];
        if (code >= 1 && code <= 0xaf) break;
        if (code == 0xb3) out.sequenceOffset = i;
        if (code == 0xb8) hasGop = true;
        if (code == 0 && out.pictureOffset < 0) {
            out.pictureOffset = i;
            const int pictureType = (packet.data[i + 5] >> 3) & 7;
            out.startsGop = pictureType == 1 && (hasGop || out.sequenceOffset >= 0 || (packet.flags & AV_PKT_FLAG_KEY));
        }
        if (code == 0xb5 && i + 6 < packet.size) {
            const int extension = packet.data[i + 4] >> 4;
            if (extension == 8 && (packet.data[i + 6] & 3) != 3) out.fieldPicture = true;
            // シーケンス先頭以外での行列更新は、直前GOPだけでは状態を再現できない。
            if (extension == 3 && (out.sequenceOffset < 0 || out.pictureOffset >= 0)) out.matrixUpdate = true;
        }
    }
    if (out.fieldPicture) out.completeFieldPair = InspectFieldPair(packet, out);
    return out;
}

// 新旧の同梱FFmpegで、フレームへ入力パケット番号を引き継ぐ。
inline void SetIdentity(AVCodecContext* codec, AVPacket* packet, int64_t number) {
#ifdef AV_CODEC_FLAG_COPY_OPAQUE
    av_buffer_unref(&packet->opaque_ref);
    packet->opaque_ref = av_buffer_alloc(sizeof(number));
    if (!packet->opaque_ref) throw std::bad_alloc();
    std::memcpy(packet->opaque_ref->data, &number, sizeof(number));
#else
    codec->reordered_opaque = number;
#endif
}

inline int64_t Identity(const AVFrame* frame) {
#ifdef AV_CODEC_FLAG_COPY_OPAQUE
    if (!frame->opaque_ref || frame->opaque_ref->size < sizeof(int64_t)) {
        throw LogoGopDecodeError("GOP復号結果に入力パケット番号がない");
    }
    int64_t number;
    std::memcpy(&number, frame->opaque_ref->data, sizeof(number));
    return number;
#else
    return frame->reordered_opaque;
#endif
}

inline void DecodeJob(AVCodecContext* codec, const std::shared_ptr<Job>& job) {
    avcodec_flush_buffers(codec);
    Frame frame(av_frame_alloc());
    if (!frame) throw std::bad_alloc();
    auto receive = [&]() {
        for (;;) {
            const int result = avcodec_receive_frame(codec, frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            Check(result, "GOPフレーム受信失敗");
            const int64_t number = Identity(frame.get());
            if (number < job->inputBegin || number >= job->outputEnd) {
                throw LogoGopDecodeError("GOP復号結果のパケット番号が入力範囲外");
            }
            if (number >= job->outputBegin) {
                Output output;
                output.packetNumber = number;
                output.sourcePosition = job->packets[size_t(number - job->inputBegin)]->pos;
                output.frame.reset(av_frame_clone(frame.get()));
                if (!output.frame) throw std::bad_alloc();
                if (!job->output.push(std::move(output))) return false;
            }
            av_frame_unref(frame.get());
        }
    };
    for (size_t i = 0; i < job->packets.size(); i++) {
        if (job->output.isStopped()) return;
        const auto& original = job->packets[i];
        std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_clone(original.get()));
        if (!packet) throw std::bad_alloc();
        // 直前GOPにシーケンスヘッダーがなければ、その時点のヘッダーを補う。
        if (i == 0 && job->inputBegin > 0 && job->sequence && Inspect(*packet).sequenceOffset < 0) {
            std::unique_ptr<AVPacket, PacketDeleter> prefixed(av_packet_alloc());
            if (!prefixed) throw std::bad_alloc();
            Check(av_new_packet(prefixed.get(), int(job->sequence->size()) + packet->size), "GOPヘッダー領域確保失敗");
            std::memcpy(prefixed->data, job->sequence->data(), job->sequence->size());
            std::memcpy(prefixed->data + job->sequence->size(), packet->data, packet->size);
            Check(av_packet_copy_props(prefixed.get(), packet.get()), "GOPパケット属性コピー失敗");
            packet = std::move(prefixed);
        }
        SetIdentity(codec, packet.get(), job->inputBegin + int64_t(i));
        const int result = avcodec_send_packet(codec, packet.get());
        Check(result, "GOPパケット送信失敗");
        if (!receive()) return;
    }
    // 最後の参照フレームも出し切る。次GOPのフレームはこのジョブには含めない。
    Check(avcodec_send_packet(codec, nullptr), "GOP終端送信失敗");
    receive();
    job->output.close();
}

} // GOP復号の内部処理

// readPacketは対象映像のパケットを順に返す。onFrameのfalseは全段の停止を要求する。
inline LogoGopDecodeReport RunLogoGopDecode(const AVCodecParameters* parameters, AVRational timebase,
    const LogoGopDecodeSettings& settings, const std::function<bool(AVPacket*)>& readPacket,
    const std::function<bool(AVFrame*, int64_t)>& onFrame) {
    using namespace gopdecode;
    if (parameters->codec_id != AV_CODEC_ID_MPEG2VIDEO || settings.decoders < 1
        || settings.threadsPerDecoder < 1 || settings.gopsPerJob < 1 || settings.outputQueueDepth < 0) {
        throw std::invalid_argument("GOP並列復号の設定が不正");
    }
    State state;
    Queue<std::shared_ptr<Job>> assignments(state, size_t(settings.decoders) * 2);
    Queue<int> permits(state, size_t(settings.decoders));
    for (int i = 0; i < settings.decoders; i++) permits.push(0);
    const int imageBytes = av_image_get_buffer_size(AVPixelFormat(parameters->format), parameters->width, parameters->height, 32);
    if (imageBytes <= 0) throw std::invalid_argument("GOP並列復号の画像サイズが不正");
    // フレーム数を明示した場合はGOP長と独立に制限し、MiBによるジョブ短縮を行わない。
    const size_t packetsPerJob = settings.outputQueueDepth > 0 ? std::numeric_limits<size_t>::max()
        : std::max(size_t(1), settings.frameBufferBytes / size_t(settings.decoders) / size_t(imageBytes));
    std::vector<std::unique_ptr<Queue<std::shared_ptr<Job>>>> inputs;
    std::vector<Codec> codecs;
    std::vector<std::thread> workers;
    std::thread splitter;
    LogoGopDecodeReport report;
    auto finish = [&]() {
        state.stop();
        if (splitter.joinable()) splitter.join();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    };
    try {
        const AVCodec* decoder = avcodec_find_decoder(parameters->codec_id);
        if (!decoder) throw LogoGopDecodeError("MPEG-2デコーダーがない");
        for (int i = 0; i < settings.decoders; i++) {
            Codec codec(avcodec_alloc_context3(decoder));
            if (!codec) throw std::bad_alloc();
            Check(avcodec_parameters_to_context(codec.get(), parameters), "GOPデコーダー設定失敗");
            codec->thread_count = settings.threadsPerDecoder;
            codec->pkt_timebase = timebase;
#ifdef AV_CODEC_FLAG_COPY_OPAQUE
            codec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
#endif
            Check(avcodec_open2(codec.get(), decoder, nullptr), "GOPデコーダー初期化失敗");
            codecs.push_back(std::move(codec));
            inputs.emplace_back(new Queue<std::shared_ptr<Job>>(state, 1));
        }
        for (int i = 0; i < settings.decoders; i++) {
            workers.emplace_back([&, i]() {
                try {
                    std::shared_ptr<Job> job;
                    while (inputs[i]->pop(job)) DecodeJob(codecs[i].get(), job);
                } catch (...) { state.stop(std::current_exception()); }
            });
        }
        splitter = std::thread([&]() {
            try {
                std::shared_ptr<std::vector<uint8_t>> sequence;
                std::shared_ptr<Gop> previous;
                auto current = std::make_shared<Gop>();
                std::vector<std::shared_ptr<Gop>> group;
                size_t groupPackets = 0;
                size_t bufferedBytes = 0;
                auto dispatch = [&]() {
                    if (group.empty()) return true;
                    int permit;
                    if (!permits.pop(permit)) return false;
                    const int outputDepth = settings.outputQueueDepth > 0 ? settings.outputQueueDepth : int(groupPackets + 1);
                    auto job = std::make_shared<Job>(state, outputDepth);
                    job->number = report.jobs++;
                    job->outputBegin = group.front()->firstPacket;
                    job->outputEnd = group.back()->firstPacket + int64_t(group.back()->packets.size());
                    const auto& first = previous ? previous : group.front();
                    job->inputBegin = first->firstPacket;
                    job->sequence = first->sequence;
                    if (previous) {
                        job->packets = previous->packets;
                        report.prerollPackets += int64_t(previous->packets.size());
                    }
                    for (const auto& gop : group) job->packets.insert(job->packets.end(), gop->packets.begin(), gop->packets.end());
                    previous = group.back();
                    group.clear();
                    groupPackets = 0;
                    bufferedBytes = 0;
                    // 統合側へ先に通知し、担当ジョブの出力キューを必ず排出できるようにする。
                    return assignments.push(job) && inputs[size_t(job->number % settings.decoders)]->push(job);
                };
                for (;;) {
                    if (state.stopped.load(std::memory_order_acquire)) return;
                    Packet packet(av_packet_alloc(), PacketDeleter());
                    if (!packet) throw std::bad_alloc();
                    if (!readPacket(packet.get())) break;
                    const auto header = Inspect(*packet);
                    if (header.fieldPicture && !header.completeFieldPair) {
                        throw LogoGopDecodeError("同一パケット内で上下が揃わないフィールドピクチャーはGOP並列復号の対象外");
                    }
                    if (header.matrixUpdate) {
                        throw LogoGopDecodeError("量子化行列の途中更新はGOP並列復号の対象外");
                    }
                    if (header.startsGop && !current->packets.empty()) {
                        if (!group.empty() && groupPackets + current->packets.size() > packetsPerJob && !dispatch()) return;
                        group.push_back(current);
                        groupPackets += current->packets.size();
                        if (int(group.size()) >= settings.gopsPerJob && !dispatch()) return;
                        current = std::make_shared<Gop>();
                        current->firstPacket = report.packets;
                    }
                    if (header.sequenceOffset >= 0 && header.pictureOffset > header.sequenceOffset) {
                        sequence = std::make_shared<std::vector<uint8_t>>(packet->data + header.sequenceOffset, packet->data + header.pictureOffset);
                    }
                    if (current->packets.empty()) current->sequence = sequence;
                    bufferedBytes += size_t(packet->size);
                    // 境界を見つけられない壊れた入力で、圧縮パケットを無制限に保持しない。
                    if (bufferedBytes > (64u << 20)) throw LogoGopDecodeError("GOP分割待ちの圧縮データが64MiBを超えた");
                    current->packets.push_back(std::move(packet));
                    report.packets++;
                }
                if (!current->packets.empty()) {
                    if (!group.empty() && groupPackets + current->packets.size() > packetsPerJob && !dispatch()) return;
                    group.push_back(current);
                    groupPackets += current->packets.size();
                }
                dispatch();
                for (auto& input : inputs) input->close();
                assignments.close();
            } catch (...) { state.stop(std::current_exception()); }
        });
        std::shared_ptr<Job> job;
        while (assignments.pop(job)) {
            Output output;
            while (job->output.pop(output)) {
                report.frames++;
                if (!onFrame(output.frame.get(), output.sourcePosition)) { state.stop(); break; }
            }
            permits.push(0);
        }
        finish();
    } catch (...) {
        finish();
        throw;
    }
    if (state.error) std::rethrow_exception(state.error);
    return report;
}

} // ロゴ処理
