#include "NicoJKAlignAudio.h"
#include "OSUtil.h"
#include "StreamReform.h"
#include "TsSplitter.h"
#include <algorithm>
#include <cmath>

namespace {

constexpr int SAMPLE_RATE = 8000;
class ReferenceAudioSplitter : public TsSplitter {
  public:
    ReferenceAudioSplitter(AMTContext& ctx, int sid) : TsSplitter(ctx, true, true, false) {
        if (sid > 0) {
            setServiceId(sid);
        }
    }

    NicoJKAlignAudio read(const tstring& path) {
        File input(path, _T("rb"));
        std::vector<uint8_t> buffer(4 * 1024 * 1024);
        while (const auto length = input.read(MemoryChunk(buffer.data(), buffer.size()))) {
            inputTsData(MemoryChunk(buffer.data(), length));
        }
        flush();
        if (!haveAudio_ || !haveVideo_) {
            THROW(FormatException, "基準録画に照合用の映像・主音声がありません");
        }
        NicoJKAlignAudio result;
        const double videoOrigin = firstVideoPTS_ + minimumVideoOffset_;
        result.duration = (maximumVideoOffset_ - minimumVideoOffset_) / MPEG_CLOCK_HZ;
        if (result.duration <= 0) {
            THROW(FormatException, "基準録画の長さを取得できません");
        }
        result.samples.resize((size_t)std::ceil(result.duration * SAMPLE_RATE), 0);
        const int64_t shift =
            (int64_t)std::llround(NicoJKAlignPtsDifference(firstAudioPTS_, videoOrigin) * SAMPLE_RATE / MPEG_CLOCK_HZ);
        for (size_t i = 0; i < audio_.size(); ++i) {
            const int64_t destination = (int64_t)i + shift;
            if (destination >= 0 && (size_t)destination < result.samples.size()) {
                result.samples[(size_t)destination] = audio_[i];
            }
        }
        result.serviceId = getActualServiceId();
        result.hasTime = haveTime_;
        if (haveTime_) {
            // StreamReformInfoのfirstFrameTime_と同じ丸め方でTOTから映像先頭へ戻す。
            const double difference = (double)(int32_t(timeClock_ / 300 - videoOrigin)) / MPEG_CLOCK_HZ;
            tm time = {};
            firstTime_.getDay(time.tm_year, time.tm_mon, time.tm_mday);
            firstTime_.getTime(time.tm_hour, time.tm_min, time.tm_sec);
            time.tm_year -= 1900;
            time.tm_mon -= 1;
            time.tm_hour -= 9;
            // 評価では丸めの±0.5秒が正解の誤差になるので、丸めない値も残す。
            const double totTime = (double)amt_mkgmtime(&time);
            result.firstFrameTimeExact = totTime - difference;
            time.tm_sec -= (int)std::round(difference);
            result.firstFrameTime = (double)amt_mkgmtime(&time);
        }
        return result;
    }

  private:
    std::vector<float> audio_;
    bool haveAudio_ = false;
    bool haveVideo_ = false;
    bool haveTime_ = false;
    int64_t firstAudioPTS_ = 0;
    int64_t firstVideoPTS_ = 0;
    int64_t timeClock_ = 0;
    double minimumVideoOffset_ = 0;
    double maximumVideoOffset_ = 0;
    JSTTime firstTime_;

    void onVideoPesPacket(int64_t, const std::vector<VideoFrameInfo>& frames, PESPacket) override {
        for (const auto& frame : frames) {
            if (frame.PTS < 0) {
                continue;
            }
            if (!haveVideo_) {
                firstVideoPTS_ = frame.PTS;
                haveVideo_ = true;
            }
            const double difference = NicoJKAlignPtsDifference(frame.PTS, (double)firstVideoPTS_);
            minimumVideoOffset_ = std::min(minimumVideoOffset_, difference);
            maximumVideoOffset_ = std::max(maximumVideoOffset_, difference);
        }
    }
    void onAudioPesPacket(int audioIdx, int64_t, const std::vector<AudioFrameData>& frames, PESPacket,
                          const AudioComponentInfo&) override {
        if (audioIdx != 0) {
            return;
        }
        for (const auto& frame : frames) {
            if (!frame.decodedData || frame.decodedDataSize <= 0 || frame.PTS < 0) {
                continue;
            }
            if (!haveAudio_) {
                firstAudioPTS_ = frame.PTS;
                haveAudio_ = true;
            }
            // faadの解析用PCMは元のチャンネル数に関係なく2chである。
            AddNicoJKAlignAudioFrame(audio_, frame.PTS, (double)firstAudioPTS_, frame.format.sampleRate,
                                     reinterpret_cast<const int16_t*>(frame.decodedData), frame.decodedDataSize / 4);
        }
    }
    bool onAudioPesReady(int audioIdx, int64_t, PESPacket, const AudioComponentInfo&) override {
        // 照合に使わない副音声はデコードしない。
        return audioIdx != 0;
    }
    void onTime(int64_t clock, JSTTime time) override {
        if (!haveTime_) {
            firstTime_ = time;
            timeClock_ = clock;
            haveTime_ = true;
        }
    }
    void onVideoFormatChanged(VideoFormat) override {
    }
    void onAudioFormatChanged(int, AudioFormat) override {
    }
    void onCaptionPesPacket(int64_t, std::vector<CaptionItem>&, PESPacket) override {
    }
    DRCSOutInfo getDRCSOutPath(int64_t, const std::string&) override {
        return DRCSOutInfo();
    }
};

}

NicoJKAlignAudio ReadNicoJKTargetAudio(const tstring& wavePath, const std::vector<FileAudioFrameInfo>& frames,
                                       double videoOriginPTS, double duration) {
    if (!std::isfinite(videoOriginPTS) || !std::isfinite(duration) || duration <= 0 || duration > 24 * 3600) {
        THROW(ArgumentException, "対象音声の原点または長さが不正です");
    }
    File input(wavePath, _T("rb"));
    std::vector<float> samples;
    std::vector<int16_t> pcm;
    for (const auto& frame : frames) {
        if (frame.audioIdx != 0 || frame.waveDataSize <= 0) {
            continue;
        }
        if (frame.waveDataSize % 4 != 0 || frame.waveOffset < 0) {
            THROW(FormatException, "照合用PCMの範囲が不正です");
        }
        if (frame.PTS < 0 || frame.format.sampleRate <= 0) {
            continue;
        }
        // 対象の範囲に入らないフレームは、PCMを読む前に捨てる。
        const double start = NicoJKAlignPtsDifference(frame.PTS, videoOriginPTS) / MPEG_CLOCK_HZ;
        const double length = frame.waveDataSize / (4.0 * frame.format.sampleRate);
        if (start >= duration || start + length <= 0) {
            continue;
        }
        pcm.resize(frame.waveDataSize / sizeof(int16_t));
        input.seek(frame.waveOffset, SEEK_SET);
        if (input.read(MemoryChunk(reinterpret_cast<uint8_t*>(pcm.data()), frame.waveDataSize)) !=
            (size_t)frame.waveDataSize) {
            THROW(FormatException, "照合用PCMが途中で終わりました");
        }
        AddNicoJKAlignAudioFrame(samples, frame.PTS, videoOriginPTS, frame.format.sampleRate, pcm.data(),
                                 pcm.size() / 2);
    }
    NicoJKAlignAudio result;
    result.samples = std::move(samples);
    result.duration = duration;
    result.samples.resize((size_t)std::ceil(duration * SAMPLE_RATE), 0);
    return result;
}

void WriteNicoJKAlignWav(const tstring& path, const std::vector<float>& samples) {
    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        const long value = std::lround((double)samples[i] * 32768.0);
        pcm[i] = (int16_t)std::max(-32768L, std::min(32767L, value));
    }
    const uint32_t dataBytes = (uint32_t)(pcm.size() * sizeof(int16_t));
    uint8_t header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                          0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    auto put32 = [&](int offset, uint32_t value) {
        for (int i = 0; i < 4; ++i) header[offset + i] = (uint8_t)(value >> (8 * i));
    };
    put32(4, 36 + dataBytes);
    put32(24, SAMPLE_RATE);
    put32(28, SAMPLE_RATE * 2);
    put32(40, dataBytes);
    // PCMはリトルエンディアンで書く(ホストがリトルエンディアンの前提)。
    File wav(path, _T("wb"));
    wav.write(MemoryChunk(header, sizeof(header)));
    wav.write(MemoryChunk((uint8_t*)pcm.data(), dataBytes));
}

NicoJKAlignAudio ReadNicoJKReferenceAudio(AMTContext& ctx, const tstring& path, int serviceId) {
    ReferenceAudioSplitter splitter(ctx, serviceId);
    return splitter.read(path);
}
