#include "NicoJKAlignAudio.h"
#include "StringUtils.h"
#include <algorithm>
#include <cmath>

namespace {

constexpr int SAMPLE_RATE = 8000;
constexpr int PTS_RATE = 90000;

}

double NicoJKAlignPtsDifference(int64_t pts, double origin) {
    return std::remainder((double)pts - origin, 8589934592.0);
}

// 8kHzの1サンプルに入る元音声を平均し、PTSの示す位置へ直接置く。
// 不明なPTSは使わず、欠落部分は0のままにする。重複した位置は後のフレームを優先する。
void AddNicoJKAlignAudioFrame(std::vector<float>& samples, int64_t pts, double originPTS, int sampleRate,
                              const int16_t* stereo, size_t count) {
    if (pts < 0 || sampleRate <= 0 || !stereo || count == 0) {
        return;
    }
    // 33bitのラップを戻し、出力位置を8kHzのサンプル数で求める。
    const double difference = NicoJKAlignPtsDifference(pts, originPTS);
    const int64_t first = (int64_t)std::llround(difference * SAMPLE_RATE / PTS_RATE);
    const int outputCount = (int)std::llround((double)count * SAMPLE_RATE / sampleRate);
    for (int i = 0; i < outputCount; ++i) {
        const int64_t destination = first + i;
        if (destination < 0) {
            continue;
        }
        if (destination > (int64_t)SAMPLE_RATE * 24 * 3600) {
            THROW(FormatException, "音声のPTSが照合可能な長さを超えています");
        }
        const size_t begin = (size_t)((int64_t)i * sampleRate / SAMPLE_RATE);
        const size_t end = std::min(count, (size_t)((int64_t)(i + 1) * sampleRate / SAMPLE_RATE));
        if (begin >= end) {
            continue;
        }
        double sum = 0;
        for (size_t j = begin; j < end; ++j) {
            sum += (int)stereo[2 * j] + stereo[2 * j + 1];
        }
        if ((size_t)destination >= samples.size()) {
            samples.resize((size_t)destination + 1, 0);
        }
        samples[(size_t)destination] = (float)(sum / ((end - begin) * 65536.0));
    }
}
