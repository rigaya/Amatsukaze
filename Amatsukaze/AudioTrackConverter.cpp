#include "AudioTrackConverter.h"
#include "AudioTrackBuilder.h"
#include "AdtsParser.h"
#include "StreamReform.h"
#include "ReaderWriterFFmpeg.h"

extern "C" {
#include "neaacdec.h"
}
#include <array>
#include <limits>

namespace {
constexpr int PREROLL_FRAMES = 2;
constexpr int POSTROLL_FRAMES = 2;
constexpr double MAX_WARM_PTS_GAP_FRAMES = 1.75;
constexpr int ADTS_HEADER_BYTES = 7;
constexpr int AAC_LC_PROFILE = 1;
constexpr float SURROUND_COEFFICIENT = 0.707f;
constexpr float DOWNMIX_NORMALIZATION = 1.0f / (1.0f + 2.0f * SURROUND_COEFFICIENT);
constexpr int MONO_BITRATE = 96000;
constexpr int STEREO_BITRATE = 192000;
constexpr int SURROUND_BITRATE = 384000;
constexpr int BITRATE_PER_CHANNEL = 64000;

int ChannelCount(AUDIO_CHANNELS layout) {
    const int count = GetAudioAdtsChannelConfiguration(layout);
    if (count < 1) THROW(FormatException, "音声変換のレイアウトが不正です");
    return count;
}

class BufferDualMonoSplitter : public DualMonoSplitter {
public:
    explicit BufferDualMonoSplitter(AMTContext& ctx) : DualMonoSplitter(ctx) {}
    std::vector<uint8_t> split(MemoryChunk chunk, int channel) {
        channel_ = channel;
        data_.clear();
        inputPacket(chunk);
        if (data_.empty()) THROW(FormatException, "音声変換用デュアルモノ分離に失敗しました");
        return std::move(data_);
    }
    void OnOutFrame(int index, MemoryChunk chunk) override {
        if (index == channel_) data_.assign(chunk.data, chunk.data + chunk.length);
    }
private:
    int channel_ = -1;
    std::vector<uint8_t> data_;
};

std::vector<uint8_t> SourcePacket(AMTContext& ctx, PacketCache& cache,
    const AudioTrackReference& ref, int sfi) {
    if (ref.frameIndex < 0 || ref.dualMonoChannel < -1 || ref.dualMonoChannel > 1) {
        THROW(FormatException, "音声変換元の参照が不正です");
    }
    const auto chunk = cache[ref.frameIndex];
    AdtsHeader header;
    if (!header.parse(chunk.data, static_cast<int>(chunk.length)) || header.profile != AAC_LC_PROFILE ||
        header.sampling_frequency_index != sfi || header.number_of_raw_data_blocks_in_frame != 0 ||
        header.frame_length != chunk.length) {
        THROW(FormatException, "音声変換元のADTSヘッダが不正です");
    }
    if (ref.dualMonoChannel >= 0) {
        if (ref.srcLayout != AUDIO_MONO) THROW(FormatException, "デュアルモノ変換元のレイアウトが不正です");
        BufferDualMonoSplitter splitter(ctx);
        return splitter.split(chunk, ref.dualMonoChannel);
    }
    if (header.channel_configuration != GetAudioAdtsChannelConfiguration(ref.srcLayout)) {
        THROW(FormatException, "音声変換元のレイアウトが一致しません");
    }
    return std::vector<uint8_t>(chunk.data, chunk.data + chunk.length);
}

class TrackDecoder {
public:
    TrackDecoder(AUDIO_CHANNELS layout, int rate) : layout_(layout), rate_(rate) {
        handle_ = NeAACDecOpen();
        if (!handle_) THROW(FormatException, "音声変換用FAADの作成に失敗しました");
        auto config = NeAACDecGetCurrentConfiguration(handle_);
        config->outputFormat = FAAD_FMT_FLOAT;
        config->downMatrix = 0;
        config->dontUpSampleImplicitSBR = 1;
        if (!NeAACDecSetConfiguration(handle_, config)) {
            NeAACDecClose(handle_);
            handle_ = nullptr;
            THROW(FormatException, "音声変換用FAADの設定に失敗しました");
        }
    }
    ~TrackDecoder() { if (handle_) NeAACDecClose(handle_); }
    TrackDecoder(const TrackDecoder&) = delete;
    TrackDecoder& operator=(const TrackDecoder&) = delete;

    std::vector<float> decode(std::vector<uint8_t>& data, bool discard = false) {
        if (!initialized_) {
            unsigned long rate = 0;
            unsigned char channels = 0;
            if (NeAACDecInit(handle_, data.data(), static_cast<unsigned long>(data.size()), &rate, &channels) < 0 ||
                static_cast<int>(rate) != rate_) {
                THROW(FormatException, "音声変換用FAADの初期化に失敗しました");
            }
            initialized_ = true;
        }
        NeAACDecFrameInfo info = {};
        auto samples = static_cast<float*>(NeAACDecDecode(handle_, &info, data.data(), static_cast<unsigned long>(data.size())));
        if (info.error || info.samplerate != static_cast<unsigned long>(rate_) || !samples) {
            THROW(FormatException, "音声変換元のデコードに失敗しました");
        }
        if (discard) return {};
        if (info.channels == 0 || info.samples != AAC_LC_FRAME_SAMPLES * info.channels) {
            THROW(FormatException, "音声変換元のPCMフレーム数が一致しません");
        }
        const int channels = ChannelCount(layout_);
        std::array<int, 6> order = {};
        if (layout_ == AUDIO_MONO) {
            // 同梱FAADのPS対応ではmonoも同値のFL/FRへ展開される。片側を論理monoへ戻す。
            if (info.channels != 1 && info.channels != 2) THROW(FormatException, "monoのデコードチャンネル数が不正です");
            if ((info.channels == 2 && (info.channel_position[0] != FRONT_CHANNEL_LEFT ||
                info.channel_position[1] != FRONT_CHANNEL_RIGHT)) ||
                (info.channels == 1 && info.channel_position[0] != FRONT_CHANNEL_CENTER)) {
                THROW(FormatException, "monoのデコードチャンネル位置が不正です");
            }
            order[0] = 0;
        } else {
            if (info.channels != channels) THROW(FormatException, "音声変換元のチャンネル数が一致しません");
            // FAADの5.1はC,L,R,BL,BR,LFE。エンコーダのWAV順FL,FR,FC,LFE,BL,BRへ並べ替える。
            // stereoはL,R。実際のchannel_positionで対応を検証し、順番だけに依存しない。
            const std::array<unsigned char, 6> positions = layout_ == AUDIO_STEREO
                ? std::array<unsigned char, 6>{ FRONT_CHANNEL_LEFT, FRONT_CHANNEL_RIGHT, 0, 0, 0, 0 }
                : std::array<unsigned char, 6>{ FRONT_CHANNEL_LEFT, FRONT_CHANNEL_RIGHT, FRONT_CHANNEL_CENTER,
                    LFE_CHANNEL, BACK_CHANNEL_LEFT, BACK_CHANNEL_RIGHT };
            if (layout_ != AUDIO_STEREO && layout_ != AUDIO_32_LFE) {
                THROW(FormatException, "音声変換元のチャンネル配置が未対応です");
            }
            for (int ch = 0; ch < channels; ++ch) {
                order[ch] = -1;
                for (int j = 0; j < info.channels; ++j) {
                    if (info.channel_position[j] == positions[ch]) order[ch] = j;
                }
                if (order[ch] < 0) THROW(FormatException, "FAADのチャンネル位置が一致しません");
            }
        }
        std::vector<float> output(AAC_LC_FRAME_SAMPLES * channels);
        for (int i = 0; i < AAC_LC_FRAME_SAMPLES; ++i) {
            for (int ch = 0; ch < channels; ++ch) output[i * channels + ch] = samples[i * info.channels + order[ch]];
        }
        return output;
    }
private:
    NeAACDecHandle handle_ = nullptr;
    AUDIO_CHANNELS layout_;
    int rate_;
    bool initialized_ = false;
};

uint64_t EncoderLayout(AUDIO_CHANNELS layout) {
    switch (layout) {
    case AUDIO_MONO: return AV_CH_LAYOUT_MONO;
    case AUDIO_STEREO: return AV_CH_LAYOUT_STEREO;
    case AUDIO_32_LFE: return AV_CH_LAYOUT_5POINT1_BACK;
    default: THROW(FormatException, "AAC変換先のレイアウトが未対応です");
    }
    return 0;
}

std::vector<uint8_t> AddAdtsHeader(const AVPacket* packet, const AudioTrackPlan& plan) {
    const int length = packet->size + ADTS_HEADER_BYTES;
    if (packet->size <= 0 || length >= (1 << 13)) THROW(FormatException, "変換AACのパケット長が不正です");
    AutoBuffer data;
    BitWriter writer(data);
    writer.write<12>(0xfff);
    writer.write<1>(0);
    writer.write<2>(0);
    writer.write<1>(1);
    writer.write<2>(AAC_LC_PROFILE);
    writer.write<4>(plan.samplingFrequencyIndex);
    writer.write<1>(0);
    writer.write<3>(GetAudioAdtsChannelConfiguration(plan.layout));
    writer.write<4>(0);
    writer.write<13>(length);
    writer.write<11>(0x7ff);
    writer.write<2>(0);
    writer.flush();
    data.add(MemoryChunk(packet->data, packet->size));
    AdtsHeader header;
    if (!header.parse(data.ptr(), static_cast<int>(data.size())) || header.profile != AAC_LC_PROFILE ||
        header.sampling_frequency_index != plan.samplingFrequencyIndex ||
        header.channel_configuration != GetAudioAdtsChannelConfiguration(plan.layout)) {
        THROW(FormatException, "変換AACとコピー側のヘッダが一致しません");
    }
    return std::vector<uint8_t>(data.ptr(), data.ptr() + data.size());
}

class TrackEncoder {
public:
    TrackEncoder(AMTContext& ctx, const AudioTrackPlan& plan, size_t expected) : plan_(plan), expected_(expected) {
        const AVCodec* codec = avcodec_find_encoder_by_name("aac");
        if (!codec) THROW(FormatException, "libavcodecのaacエンコーダが見つかりません");
        context_.Set(codec);
        auto context = context_();
        context->sample_rate = plan.sampleRate;
        context->sample_fmt = AV_SAMPLE_FMT_FLTP;
        context->profile = FF_PROFILE_AAC_LOW;
        context->bit_rate = plan.layout == AUDIO_MONO ? MONO_BITRATE
            : plan.layout == AUDIO_STEREO ? STEREO_BITRATE
            : plan.layout == AUDIO_32_LFE ? SURROUND_BITRATE : BITRATE_PER_CHANNEL * ChannelCount(plan.layout);
        context->time_base = AVRational{ 1, plan.sampleRate };
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
#if LIBAVCODEC_VERSION_MAJOR >= 59
        if (av_channel_layout_from_mask(&context->ch_layout, EncoderLayout(plan.layout)) < 0) {
            THROW(FormatException, "AACエンコーダのチャンネル配置設定に失敗しました");
        }
#else
        context->channel_layout = EncoderLayout(plan.layout);
        context->channels = ChannelCount(plan.layout);
#endif
        if (avcodec_open2(context, codec, nullptr) < 0 || context->frame_size != AAC_LC_FRAME_SAMPLES ||
            context->initial_padding < 0 || context->extradata_size < 2) {
            THROW(FormatException, "AACエンコーダの初期化に失敗しました");
        }
        // AudioSpecificConfig自体を検査する。ADTSを付けただけで互換と判断しない。
        const int objectType = context->extradata[0] >> 3;
        const int sfi = ((context->extradata[0] & 7) << 1) | (context->extradata[1] >> 7);
        const int config = (context->extradata[1] >> 3) & 15;
        if (objectType != AAC_LC_PROFILE + 1 || sfi != plan.samplingFrequencyIndex ||
            config != GetAudioAdtsChannelConfiguration(plan.layout)) {
            THROW(FormatException, "AACエンコーダ設定とコピー側のヘッダが一致しません");
        }
        const int delay = context->initial_padding;
        const int padding = (AAC_LC_FRAME_SAMPLES - delay % AAC_LC_FRAME_SAMPLES) % AAC_LC_FRAME_SAMPLES;
        discard_ = (static_cast<int64_t>(padding) + delay) / AAC_LC_FRAME_SAMPLES + PREROLL_FRAMES;
        ctx.infoF(_T("AAC変換設定: d=%d p=%d 先頭破棄=%lld bitrate=%lld"), delay, padding,
            static_cast<long long>(discard_), static_cast<long long>(context->bit_rate));
        pending_.assign(padding * ChannelCount(plan.layout), 0);
        auto frame = frame_();
        frame->format = context->sample_fmt;
        frame->sample_rate = context->sample_rate;
        frame->nb_samples = AAC_LC_FRAME_SAMPLES;
#if LIBAVCODEC_VERSION_MAJOR >= 59
        if (av_channel_layout_copy(&frame->ch_layout, &context->ch_layout) < 0) {
            THROW(FormatException, "AAC入力のチャンネル配置設定に失敗しました");
        }
#else
        frame->channel_layout = context->channel_layout;
        frame->channels = context->channels;
#endif
        if (av_frame_get_buffer(frame, 0) < 0) THROW(FormatException, "AAC入力バッファの確保に失敗しました");
    }
    void input(const std::vector<float>& pcm) {
        const int channels = ChannelCount(plan_.layout);
        if (pcm.size() % channels) THROW(FormatException, "AAC入力のPCM長が不正です");
        pending_.insert(pending_.end(), pcm.begin(), pcm.end());
        const size_t frameValues = AAC_LC_FRAME_SAMPLES * channels;
        size_t consumed = 0;
        while (pending_.size() - consumed >= frameValues) {
            auto frame = frame_();
            if (av_frame_make_writable(frame) < 0) THROW(FormatException, "AAC入力バッファが書き込みできません");
            for (int ch = 0; ch < channels; ++ch) {
                auto output = reinterpret_cast<float*>(frame->extended_data[ch]);
                for (int i = 0; i < AAC_LC_FRAME_SAMPLES; ++i) output[i] = pending_[consumed + i * channels + ch];
            }
            frame->pts = pts_;
            pts_ += AAC_LC_FRAME_SAMPLES;
            if (avcodec_send_frame(context_(), frame) < 0) THROW(FormatException, "AACのエンコードに失敗しました");
            receive();
            consumed += frameValues;
        }
        pending_.erase(pending_.begin(), pending_.begin() + consumed);
    }
    std::vector<std::vector<uint8_t>> finish() {
        if (!pending_.empty()) {
            const int channels = ChannelCount(plan_.layout);
            std::vector<float> pad(AAC_LC_FRAME_SAMPLES * channels - pending_.size(), 0);
            input(pad);
        }
        if (avcodec_send_frame(context_(), nullptr) < 0) THROW(FormatException, "AACの終了処理に失敗しました");
        receive();
        if (output_.size() != expected_) THROW(FormatException, "変換AACのフレーム数が不足しています");
        return std::move(output_);
    }
private:
    void receive() {
        auto packet = std::unique_ptr<AVPacket, void(*)(AVPacket*)>(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
        if (!packet) THROW(FormatException, "AACパケットの確保に失敗しました");
        while (true) {
            const int result = avcodec_receive_packet(context_(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) THROW(FormatException, "AACパケットの取得に失敗しました");
            if (packet->duration != 0 && packet->duration != AAC_LC_FRAME_SAMPLES) {
                THROW(FormatException, "変換AACのパケット時間が不正です");
            }
            if (packetIndex_++ >= discard_ && output_.size() < expected_) output_.push_back(AddAdtsHeader(packet.get(), plan_));
            av_packet_unref(packet.get());
        }
    }
    const AudioTrackPlan& plan_;
    size_t expected_;
    av::CodecContext context_;
    av::Frame frame_;
    std::vector<float> pending_;
    std::vector<std::vector<uint8_t>> output_;
    int64_t pts_ = 0;
    int64_t discard_ = 0;
    int64_t packetIndex_ = 0;
};

AudioTrackReference SourceReference(int index, const AudioTrackPlan& plan,
    const std::vector<FileAudioFrameInfo>& info) {
    AudioTrackReference ref;
    ref.frameIndex = index;
    ref.srcLayout = info[index].format.channels;
    if (ref.srcLayout == AUDIO_2LANG) {
        ref.srcLayout = AUDIO_MONO;
        ref.dualMonoChannel = plan.logicalTrack;
    } else if (plan.logicalTrack == 1) {
        ref.frameIndex = -1;
    }
    return ref;
}

void WarmDecoder(AMTContext& ctx, TrackDecoder& decoder, PacketCache& cache, const AudioTrackPlan& plan,
    const AudioTrackReference& ref, const std::vector<int>& source, size_t position,
    const std::vector<FileAudioFrameInfo>& info) {
    // FAADの初回出力だけを捨てる。プリロール自身のPCMはエンコーダへ渡す。
    auto warm = GenerateSilentAdtsFrame(ref.srcLayout, plan.samplingFrequencyIndex);
    if (position > 0) {
        const int index = source[position - 1];
        const auto candidate = SourceReference(index, plan, info);
        const double ptsGap = static_cast<double>(info[ref.frameIndex].PTS) - info[index].PTS;
        const double maximumGap = MAX_WARM_PTS_GAP_FRAMES * AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / plan.sampleRate;
        // 長欠落前の古い音声を履歴に入れない。PTS不明時は元packetの順序を使う。
        const bool continuous = info[ref.frameIndex].PTS < 0 || info[index].PTS < 0 || (ptsGap >= 0 && ptsGap <= maximumGap);
        if (candidate.frameIndex >= 0 && candidate.srcLayout == ref.srcLayout && continuous &&
            candidate.dualMonoChannel == ref.dualMonoChannel && info[index].format.sampleRate == plan.sampleRate &&
            info[index].numSamples == AAC_LC_FRAME_SAMPLES) {
            warm = SourcePacket(ctx, cache, candidate, plan.samplingFrequencyIndex);
        }
    }
    decoder.decode(warm, true);
}
}

std::vector<float> ConvertAudioChannels(const std::vector<float>& pcm, AUDIO_CHANNELS src, AUDIO_CHANNELS dst) {
    const int inputChannels = ChannelCount(src);
    const int outputChannels = ChannelCount(dst);
    if (pcm.size() % inputChannels) THROW(FormatException, "チャンネル変換のPCM長が不正です");
    if (src == dst) return pcm;
    const bool supported = (src == AUDIO_STEREO && dst == AUDIO_32_LFE) ||
        (src == AUDIO_32_LFE && dst == AUDIO_STEREO) || (src == AUDIO_MONO && dst == AUDIO_STEREO) ||
        (src == AUDIO_STEREO && dst == AUDIO_MONO);
    if (!supported) THROW(FormatException, "チャンネル変換の組み合わせが未対応です");
    std::vector<float> output(pcm.size() / inputChannels * outputChannels, 0);
    for (size_t i = 0; i < pcm.size() / inputChannels; ++i) {
        const auto input = pcm.data() + i * inputChannels;
        auto target = output.data() + i * outputChannels;
        if (src == AUDIO_32_LFE) {
            target[0] = (input[0] + SURROUND_COEFFICIENT * (input[2] + input[4])) * DOWNMIX_NORMALIZATION;
            target[1] = (input[1] + SURROUND_COEFFICIENT * (input[2] + input[5])) * DOWNMIX_NORMALIZATION;
        } else if (dst == AUDIO_MONO) {
            target[0] = (input[0] + input[1]) / 2;
        } else if (src == AUDIO_MONO) {
            target[0] = target[1] = input[0];
        } else {
            target[0] = input[0];
            target[1] = input[1];
        }
    }
    return output;
}

std::vector<std::vector<uint8_t>> ConvertAudioTrackRun(AMTContext& ctx, PacketCache& cache,
    const AudioTrackPlan& plan, size_t begin, size_t end, const std::vector<FileAudioFrameInfo>& frameInfo) {
    if (begin >= end || end > plan.frames.size() || frameInfo.empty()) {
        THROW(FormatException, "音声変換区間または元フレーム情報が不正です");
    }
    const auto& first = plan.frames[begin];
    std::vector<int> source;
    for (size_t i = 0; i < frameInfo.size(); ++i) {
        if (frameInfo[i].audioIdx == plan.sourceTrack) source.push_back(static_cast<int>(i));
    }
    TrackEncoder encoder(ctx, plan, end - begin);
    std::unique_ptr<TrackDecoder> decoder;
    AUDIO_CHANNELS decoderLayout = AUDIO_NONE;
    int decoderLanguage = -1;
    size_t previousPosition = source.size();
    auto inputFrame = [&](int64_t outputPosition) {
        if (outputPosition < 0 || outputPosition >= static_cast<int64_t>(plan.frames.size()) ||
            plan.frames[outputPosition].operation == AudioTrackOperation::SILENCE) {
            encoder.input(std::vector<float>(AAC_LC_FRAME_SAMPLES * ChannelCount(plan.layout), 0));
            decoder.reset();
            previousPosition = source.size();
            return;
        }
        const auto& ref = plan.frames[outputPosition];
        if (ref.frameIndex < 0 || static_cast<size_t>(ref.frameIndex) >= frameInfo.size() ||
            ref.dstLayout != plan.layout || frameInfo[ref.frameIndex].audioIdx != plan.sourceTrack ||
            frameInfo[ref.frameIndex].format.sampleRate != plan.sampleRate ||
            frameInfo[ref.frameIndex].numSamples != AAC_LC_FRAME_SAMPLES) {
            THROW(FormatException, "音声変換の隣接フレーム参照が不正です");
        }
        const auto original = SourceReference(ref.frameIndex, plan, frameInfo);
        if (original.srcLayout != ref.srcLayout || original.dualMonoChannel != ref.dualMonoChannel) {
            THROW(FormatException, "音声変換参照と元フォーマットが一致しません");
        }
        const auto position = std::lower_bound(source.begin(), source.end(), ref.frameIndex);
        if (position == source.end() || *position != ref.frameIndex) THROW(FormatException, "音声変換元のトラックが一致しません");
        const size_t currentPosition = position - source.begin();
        // 出力の隣接フレームはCOPY側の別レイアウトでもよい。切替とCMカットでは専用FAADを再作成する。
        if (!decoder || decoderLayout != ref.srcLayout || decoderLanguage != ref.dualMonoChannel ||
            (currentPosition != previousPosition && currentPosition != previousPosition + 1)) {
            decoder = std::make_unique<TrackDecoder>(ref.srcLayout, plan.sampleRate);
            WarmDecoder(ctx, *decoder, cache, plan, ref, source, currentPosition, frameInfo);
            decoderLayout = ref.srcLayout;
            decoderLanguage = ref.dualMonoChannel;
        }
        auto packet = SourcePacket(ctx, cache, ref, plan.samplingFrequencyIndex);
        encoder.input(ConvertAudioChannels(decoder->decode(packet), ref.srcLayout, plan.layout));
        previousPosition = currentPosition;
    };
    // 前後2枚は元ストリームの隣ではなく、カット・無音を反映した出力トラック上の隣を使う。
    for (int64_t i = static_cast<int64_t>(begin) - PREROLL_FRAMES; i < static_cast<int64_t>(begin); ++i) inputFrame(i);
    for (size_t i = begin; i < end; ++i) {
        const auto& ref = plan.frames[i];
        if (ref.operation != AudioTrackOperation::CONVERT || ref.srcLayout != first.srcLayout ||
            ref.dualMonoChannel != first.dualMonoChannel) THROW(FormatException, "連続音声変換区間の参照が一致しません");
        inputFrame(static_cast<int64_t>(i));
    }
    for (int64_t i = static_cast<int64_t>(end); i < static_cast<int64_t>(end) + POSTROLL_FRAMES; ++i) inputFrame(i);
    auto output = encoder.finish();
    ctx.infoF(_T("音声変換: %s 開始=%zu N=%zu src=%d dst=%d rate=%d"), plan.name.c_str(), begin, end - begin,
        ChannelCount(first.srcLayout), ChannelCount(plan.layout), plan.sampleRate);
    return output;
}
