#include "AudioTrackConverter.h"
#include "AudioTrackBuilder.h"
#include "StreamReform.h"
#include "AdtsParser.h"
#include <cmath>
#include <fstream>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
}

// 単独検証ではStreamReform全体をリンクせず、同じフレーム初期値を使う。
FileAudioFrameInfo::FileAudioFrameInfo()
    : AudioFrameInfo(), audioIdx(0), codedDataSize(0), waveDataSize(0), fileOffset(0), waveOffset(-1) {}

static void Check(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

static void CheckChannels() {
    auto stereo = ConvertAudioChannels({ 0.25f, -0.5f }, AUDIO_STEREO, AUDIO_32_LFE);
    Check(stereo.size() == 6, "ステレオから5.1へのサンプル数が不正です");
    Check(stereo[0] == 0.25f && stereo[1] == -0.5f, "左右チャンネルの位置が不正です");
    for (int i = 2; i < 6; i++) Check(stereo[i] == 0, "追加チャンネルが無音になりません");
    auto mono = ConvertAudioChannels({ 0.25f }, AUDIO_MONO, AUDIO_STEREO);
    Check(mono.size() == 2 && mono[0] == mono[1], "モノラルから左右への展開が不正です");
    auto folded = ConvertAudioChannels({ 0.25f, -0.5f }, AUDIO_STEREO, AUDIO_MONO);
    Check(folded.size() == 1 && folded[0] == -0.125f, "左右の平均がモノラルになりません");
    const std::vector<float> ordered = { 1, 2, 3, 4, 5, 6 };
    Check(ConvertAudioChannels(ordered, AUDIO_32_LFE, AUDIO_32_LFE) == ordered, "同じレイアウトで順番が変化しました");
    for (bool reverse : { false, true }) {
        bool rejected = false;
        try {
            ConvertAudioChannels(reverse ? ordered : std::vector<float>{ 1 },
                reverse ? AUDIO_32_LFE : AUDIO_MONO, reverse ? AUDIO_MONO : AUDIO_32_LFE);
        } catch (const FormatException&) { rejected = true; }
        Check(rejected, "未対応のモノラルと5.1の変換が拒否されません");
    }
    // チャンネルごとのインパルス応答を保存し、Python側で係数と順番を検証する。
    for (int channel = 0; channel < 6; channel++) {
        std::vector<float> impulse(6, 0); impulse[channel] = 1;
        auto down = ConvertAudioChannels(impulse, AUDIO_32_LFE, AUDIO_STEREO);
        Check(down.size() == 2, "5.1からステレオへのサンプル数が不正です");
        std::cout << "係数 " << channel << " " << down[0] << " " << down[1] << "\n";
    }
}

int main(int argc, char** argv) {
    try {
        Check(avcodec_find_encoder_by_name("aac") != nullptr, "リンクしたlibavcodecにAACエンコーダがありません");
        CheckChannels();
        if (argc == 1) return 0;
        Check(argc == 5 || argc == 6, "入力・参照表・出力・レイアウトの指定が必要です");
        AMTContext ctx;
        std::ifstream input(argv[1], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        std::vector<int64_t> offsets;
        std::vector<FileAudioFrameInfo> info;
        size_t position = 0;
        while (position < bytes.size()) {
            AdtsHeader header;
            Check(header.parse(bytes.data() + position, bytes.size() - position), "入力ADTSを解析できません");
            Check(header.frame_length > 0 && position + header.frame_length <= bytes.size(), "入力ADTSの長さが不正です");
            offsets.push_back(position);
            info.emplace_back();
            auto& frame = info.back();
            const int channels = header.channel_configuration;
            Check(channels == 0 || channels == 1 || channels == 2 || channels == 6, "検証素材のレイアウトが未対応です");
            frame.format = { channels == 0 ? AUDIO_2LANG : channels == 1 ? AUDIO_MONO : channels == 2 ? AUDIO_STEREO : AUDIO_32_LFE, 48000 };
            frame.numSamples = AAC_LC_FRAME_SAMPLES;
            frame.PTS = (info.size() - 1) * AAC_LC_FRAME_SAMPLES * MPEG_CLOCK_HZ / 48000;
            frame.codedDataSize = header.frame_length;
            frame.fileOffset = position;
            position += header.frame_length;
        }
        offsets.push_back(bytes.size());
        // 任意のPTS表で、トラックが長時間欠落する入力の物理フレーム時刻を再現する。
        std::ifstream pts(std::string(argv[1]) + ".pts");
        if (pts) {
            for (auto& frame : info) {
                Check(static_cast<bool>(pts >> frame.PTS), "PTS表の要素数が不足しています");
            }
            int64_t extra;
            Check(!(pts >> extra), "PTS表の要素数が多すぎます");
        }
        PacketCache cache(ctx, char_to_tstring(argv[1]), offsets, 4, 8);
        AudioTrackPlan plan;
        plan.logicalTrack = argc == 6 ? std::stoi(argv[5]) : 0;
        const int channels = std::stoi(argv[4]);
        plan.layout = channels == 1 ? AUDIO_MONO : channels == 2 ? AUDIO_STEREO : AUDIO_32_LFE;
        plan.sampleRate = 48000;
        plan.samplingFrequencyIndex = 3;
        std::ifstream refs(argv[2]);
        int index, convert;
        while (refs >> index >> convert) {
            Check(index >= 0 && index < static_cast<int>(info.size()), "参照番号が不正です");
            AudioTrackReference ref;
            ref.operation = convert ? AudioTrackOperation::CONVERT : AudioTrackOperation::COPY;
            ref.frameIndex = index;
            ref.srcLayout = info[index].format.channels;
            ref.dstLayout = plan.layout;
            if (info[index].format.channels == AUDIO_2LANG) {
                Check(argc == 6, "デュアルモノの選択チャンネル指定が必要です");
                ref.dualMonoChannel = std::stoi(argv[5]);
                ref.srcLayout = AUDIO_MONO;
            }
            plan.frames.push_back(ref);
        }
        BuildAudioTrack(ctx, cache, plan, char_to_tstring(argv[3]), info);
        std::cout << "出力フレーム数 " << plan.frames.size() << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    } catch (const Exception&) {
        return 2;
    }
}
