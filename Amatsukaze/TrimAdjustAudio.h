#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace trimadjust {

static constexpr uint32_t AUDIO_PRIMARY_METADATA_MAGIC = 0x31415054;

// dat末尾の拡張を読む。拡張がない旧datは空配列へフォールバックする。
template<class Reader>
inline bool readAudioPrimaryFlags(int64_t remaining, size_t frameCount, Reader read, std::vector<uint8_t>& flags) {
    flags.clear();
    if (remaining < static_cast<int64_t>(sizeof(uint32_t))) return true;
    uint32_t magic;
    read(&magic, sizeof(magic));
    if (magic != AUDIO_PRIMARY_METADATA_MAGIC) return true;
    if (remaining < static_cast<int64_t>(sizeof(uint32_t) + sizeof(int64_t))) return false;
    int64_t count;
    read(&count, sizeof(count));
    if (count < 0 || static_cast<uint64_t>(count) != frameCount
        || count > remaining - static_cast<int64_t>(sizeof(uint32_t) + sizeof(int64_t))) return false;
    flags.resize(static_cast<size_t>(count));
    if (count > 0) read(flags.data(), static_cast<size_t>(count));
    return true;
}

inline bool audioPrimaryOnly(const std::vector<uint8_t>& flags, size_t index, bool fallback) {
    return flags.empty() ? fallback : flags[index] != 0;
}


// 波形と再生の両方で、映像先頭を基準にした時刻を使う。
template<class Frames>
inline double audioFrameStartSeconds(const Frames& frames, int index) {
    return static_cast<double>(frames[index].framePTS - frames[0].framePTS) / 90000.0;
}

template<class Frames>
inline double audioFrameDurationSeconds(const Frames& frames, int index) {
    if (index + 1 < static_cast<int>(frames.size())) {
        return static_cast<double>(frames[index + 1].framePTS - frames[index].framePTS) / 90000.0;
    }
    return index > 0
        ? static_cast<double>(frames[index].framePTS - frames[index - 1].framePTS) / 90000.0
        : 1.0 / 30.0;
}

// 終端フレームを含める。音声の有無とは独立して範囲を検証する。
template<class Frames>
inline bool audioPlaybackRange(const Frames& frames, int start, int end, double& startSec, double& endSec) {
    if (start < 0 || end < start || end >= static_cast<int>(frames.size())) return false;
    startSec = audioFrameStartSeconds(frames, start);
    endSec = audioFrameStartSeconds(frames, end) + audioFrameDurationSeconds(frames, end);
    return std::isfinite(startSec) && std::isfinite(endSec)
        && startSec >= 0 && endSec > startSec && endSec - startSec <= 120.0 + 1e-9;
}

// 読込関数にはファイル位置、出力先、バイト数を渡す。欠損フレームは無音で埋める。
template<class Frames, class Reader, class PrimarySelector>
inline bool readAudioPcm(const Frames& frames, int samplesPerFrame, int sampleRate,
    double startSec, double endSec, PrimarySelector primaryOnly, Reader read, std::vector<int16_t>& pcm) {
    pcm.clear();
    if (frames.empty() || samplesPerFrame <= 0 || sampleRate <= 0
        || !std::isfinite(startSec) || !std::isfinite(endSec)
        || endSec - startSec > 120.0 + 1e-9) return false;
    const int64_t total = static_cast<int64_t>(samplesPerFrame) * frames.size();
    const int64_t start = static_cast<int64_t>(std::max(0.0, std::min(startSec * sampleRate, static_cast<double>(total))));
    const int64_t requestedEnd = static_cast<int64_t>(std::max(0.0, std::min(endSec * sampleRate, static_cast<double>(total))));
    // 120秒ちょうどの端点が浮動小数点の丸めで1サンプル多くなっても受け付ける。
    const int64_t end = std::min(requestedEnd, start + static_cast<int64_t>(sampleRate) * 120);
    if (start >= end) return false;
    pcm.assign(static_cast<size_t>(end - start) * 2, 0);
    for (int64_t index = start / samplesPerFrame; index <= (end - 1) / samplesPerFrame; index++) {
        const auto& frame = frames[static_cast<size_t>(index)];
        const int64_t frameStart = index * samplesPerFrame;
        const int64_t overlapStart = std::max(start, frameStart);
        const int64_t overlapEnd = std::min(end, frameStart + samplesPerFrame);
        const int64_t availableEnd = std::min(overlapEnd, frameStart + std::max(0, frame.waveLength / 4));
        if (overlapStart >= availableEnd) continue;
        auto* dest = pcm.data() + static_cast<size_t>(overlapStart - start) * 2;
        const size_t count = static_cast<size_t>(availableEnd - overlapStart);
        read(frame.waveOffset + (overlapStart - frameStart) * 4, dest, count * 4);
        if (primaryOnly(static_cast<size_t>(index))) {
            for (size_t i = 0; i < count; i++) dest[i * 2 + 1] = dest[i * 2];
        }
    }
    return true;
}

}
