#include "../../Amatsukaze/TrimAdjustAudio.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
struct VideoFrame { int64_t framePTS; };
struct AudioFrame { int64_t waveOffset; int waveLength; };
void check(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
}

int main() {
    using namespace trimadjust;
    try {
        const std::vector<VideoFrame> video = {{90000}, {93000}, {96000}};
        double start, end;
        check(audioPlaybackRange(video, 0, 1, start, end), "通常範囲を受け付ける");
        check(start == 0 && std::abs(end - 2.0 / 30) < 1e-10, "終端フレームを含める");
        check(audioPlaybackRange(video, 2, 2, start, end) && std::abs(end - .1) < 1e-10, "最終フレームの長さを補う");
        check(!audioPlaybackRange(video, -1, 0, start, end), "負の開始を拒否する");
        check(!audioPlaybackRange(video, 2, 1, start, end), "逆順を拒否する");
        check(!audioPlaybackRange(video, 0, 3, start, end), "範囲外を拒否する");
        std::vector<VideoFrame> longVideo;
        for (int i = 0; i <= 3700; i++) longVideo.push_back({90000 + i * 3000});
        check(audioPlaybackRange(longVideo, 0, 3599, start, end), "120秒を受け付ける");
        check(!audioPlaybackRange(longVideo, 0, 3600, start, end), "120秒超を拒否する");

        check(audioPlaybackRange(longVideo, 69, 3668, start, end), "途中位置から120秒を受け付ける");
        const std::vector<AudioFrame> longAudio(6000, {0, 4096});
        std::vector<int16_t> longPcm;
        check(readAudioPcm(longAudio, 1024, 48000, start, end,
            [](size_t) { return false; }, [](int64_t, int16_t*, size_t) {}, longPcm),
            "120秒境界のサンプル丸め誤差で音声なしにならない");
        check(longPcm.size() == 48000 * 120 * 2, "丸め誤差があっても120秒分を返す");
        check(!readAudioPcm(longAudio, 1024, 48000, start, end + 1.0 / 30,
            [](size_t) { return false; }, [](int64_t, int16_t*, size_t) {}, longPcm),
            "実際の120秒超過は拒否する");

        // ファイル上の音声フレーム間には無関係な領域がある。欠損と短いPCMも含める。
        const std::vector<int16_t> wave = {1, 101, 2, 102, 3, 103, 4, 104, -1, -1, 5, 105, 6, 106};
        const std::vector<AudioFrame> audio = {{0, 16}, {20, 8}, {0, 0}};
        std::vector<int16_t> pcm;
        auto reader = [&](int64_t offset, int16_t* dest, size_t bytes) {
            check(offset >= 0 && offset + bytes <= wave.size() * 2, "PCMの範囲内だけを読む");
            std::memcpy(dest, reinterpret_cast<const uint8_t*>(wave.data()) + offset, bytes);
        };
        auto stereo = [](size_t) { return false; };
        check(readAudioPcm(audio, 4, 10, .2, 1.2, stereo, reader, pcm), "境界をまたぐ読込に成功する");
        const std::vector<int16_t> expected = {3,103,4,104,5,105,6,106,0,0,0,0,0,0,0,0,0,0,0,0};
        check(pcm == expected, "位置と欠損の無音を保つ");
        const std::vector<uint8_t> primary = {0, 1, 0};
        check(readAudioPcm(audio, 4, 10, .2, .6,
            [&](size_t i) { return audioPrimaryOnly(primary, i, false); }, reader, pcm), "途中の二重音声切替を読む");
        check(pcm == std::vector<int16_t>({3,103,4,104,5,5,6,6}), "二重音声の主音声だけを両チャンネルへ出す");
        std::vector<uint8_t> datTail(sizeof(uint32_t) + sizeof(int64_t) + primary.size());
        const uint32_t magic = AUDIO_PRIMARY_METADATA_MAGIC;
        int64_t count = static_cast<int64_t>(primary.size());
        std::memcpy(datTail.data(), &magic, sizeof(magic));
        std::memcpy(datTail.data() + sizeof(magic), &count, sizeof(count));
        std::memcpy(datTail.data() + sizeof(magic) + sizeof(count), primary.data(), primary.size());
        size_t pos = 0;
        auto datReader = [&](void* dest, size_t bytes) {
            check(pos + bytes <= datTail.size(), "dat拡張の範囲内だけを読む");
            std::memcpy(dest, datTail.data() + pos, bytes);
            pos += bytes;
        };
        std::vector<uint8_t> flags;
        check(readAudioPrimaryFlags(datTail.size(), audio.size(), datReader, flags) && flags == primary,
            "新datからフレームごとの音声構成を読む");
        check(readAudioPrimaryFlags(0, audio.size(), datReader, flags) && flags.empty(),
            "旧datの終端を受け付ける");
        pos = 0;
        check(!readAudioPrimaryFlags(datTail.size() - 1, audio.size(), datReader, flags), "途切れたdat拡張を拒否する");
        pos = 0;
        check(!readAudioPrimaryFlags(datTail.size(), audio.size() + 1, datReader, flags), "datのフレーム数不一致を拒否する");
        check(audioPrimaryOnly({}, 0, true) && !audioPrimaryOnly({}, 0, false), "旧datは元の音声構成へフォールバックする");
        check(!readAudioPcm(audio, 4, 10, 2, 3, stereo, reader, pcm), "音声範囲外は空にする");
        check(!readAudioPcm(audio, 4, 0, 0, 1, stereo, reader, pcm), "不正なレートを拒否する");
        std::cout << "音声切出テスト成功\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
