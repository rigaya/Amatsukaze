#include "AudioTrackBuilder.h"
#include "AdtsParser.h"

namespace {
constexpr int ADTS_HEADER_BYTES = 7;
constexpr int AAC_LC_PROFILE = 1;
constexpr int SILENT_GLOBAL_GAIN = 100;

void WriteSilentIcs(BitWriter& writer) {
    writer.write<8>(SILENT_GLOBAL_GAIN);
    writer.write<1>(0);
    writer.write<2>(0); // 長窓のみを使用する。
    writer.write<1>(0);
    writer.write<6>(0); // スペクトル係数を持たない。
    writer.write<1>(0);
    writer.write<1>(0);
    writer.write<1>(0);
    writer.write<1>(0);
}

class TrackDualMonoSplitter : public DualMonoSplitter {
public:
    TrackDualMonoSplitter(AMTContext& ctx, File& file) : DualMonoSplitter(ctx), file_(file) {}
    void write(MemoryChunk chunk, int channel) {
        channel_ = channel;
        written_ = false;
        inputPacket(chunk);
        if (!written_) THROW(FormatException, "デュアルモノ音声の分離に失敗しました");
    }
    void OnOutFrame(int index, MemoryChunk chunk) override {
        if (index == channel_) {
            file_.write(chunk);
            written_ = true;
        }
    }
private:
    File& file_;
    int channel_ = -1;
    bool written_ = false;
};
}

std::vector<uint8_t> GenerateSilentAdtsFrame(AUDIO_CHANNELS layout, int samplingFrequencyIndex) {
    const int config = GetAudioAdtsChannelConfiguration(layout);
    if (config < 0 || samplingFrequencyIndex < 0 || samplingFrequencyIndex > 12) {
        THROW(FormatException, "無音AACのフォーマットが未対応です");
    }
    AutoBuffer raw;
    BitWriter writer(raw);
    int sce = 0, cpe = 0;
    auto element = [&](int type) {
        writer.write<3>(type);
        writer.write<4>(type == ID_CPE ? cpe++ : type == ID_SCE ? sce++ : 0);
        if (type == ID_CPE) writer.write<1>(0);
        WriteSilentIcs(writer);
        if (type == ID_CPE) WriteSilentIcs(writer);
    };
    if (config == 1) element(ID_SCE);
    else if (config == 2) element(ID_CPE);
    else {
        element(ID_SCE);
        element(ID_CPE);
        if (config == 4) element(ID_SCE);
        if (config >= 5) element(ID_CPE);
        if (config == 6) element(ID_LFE);
    }
    writer.write<3>(ID_END);
    writer.byteAlign<false>();
    writer.flush();
    const int length = ADTS_HEADER_BYTES + static_cast<int>(raw.size());
    AutoBuffer result;
    BitWriter header(result);
    header.write<12>(0xfff);
    header.write<1>(0);
    header.write<2>(0);
    header.write<1>(1);
    header.write<2>(AAC_LC_PROFILE);
    header.write<4>(samplingFrequencyIndex);
    header.write<1>(0);
    header.write<3>(config);
    header.write<4>(0);
    header.write<13>(length);
    header.write<11>(0x7ff);
    header.write<2>(0);
    header.flush();
    result.add(raw.get());
    return std::vector<uint8_t>(result.ptr(), result.ptr() + result.size());
}

void BuildAudioTrack(AMTContext& ctx, PacketCache& cache, const AudioTrackPlan& plan, const tstring& path) {
    auto silence = GenerateSilentAdtsFrame(plan.layout, plan.samplingFrequencyIndex);
    File file(path, _T("wb"));
    TrackDualMonoSplitter splitter(ctx, file);
    for (const auto& ref : plan.frames) {
        if (ref.dstLayout != plan.layout) THROW(FormatException, "出力音声のレイアウトが一致しません");
        switch (ref.operation) {
        case AudioTrackOperation::SILENCE:
            file.write(MemoryChunk(silence.data(), silence.size()));
            break;
        case AudioTrackOperation::COPY: {
            if (ref.frameIndex < 0 || ref.srcLayout != plan.layout || ref.dualMonoChannel < -1) {
                THROW(FormatException, "音声コピー参照が不正です");
            }
            auto chunk = cache[ref.frameIndex];
            AdtsHeader header;
            if (!header.parse(chunk.data, static_cast<int>(chunk.length)) || header.profile != AAC_LC_PROFILE ||
                header.sampling_frequency_index != plan.samplingFrequencyIndex ||
                header.number_of_raw_data_blocks_in_frame != 0 || header.frame_length != chunk.length ||
                (header.channel_configuration == 0 && ref.dualMonoChannel < 0)) {
                THROW(FormatException, "音声コピー元のADTSヘッダが不正です");
            }
            if (ref.dualMonoChannel >= 0) {
                if (plan.layout != AUDIO_MONO || ref.dualMonoChannel > 1) {
                    THROW(FormatException, "デュアルモノ音声の参照が不正です");
                }
                splitter.write(chunk, ref.dualMonoChannel);
            } else {
                if (header.channel_configuration != GetAudioAdtsChannelConfiguration(plan.layout)) {
                    THROW(FormatException, "音声コピー元のレイアウトが一致しません");
                }
                file.write(chunk);
            }
            break;
        }
        case AudioTrackOperation::CONVERT:
            THROW(InvalidOperationException, "音声の再エンコードは未実装です");
            break;
        default:
            THROW(FormatException, "音声トラックの出力操作が不正です");
        }
    }
}
