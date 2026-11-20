#pragma once

#include "CoreUtils.hpp"
#include <cstdint>

class AMTContext;
struct FileAudioFrameInfo;

// samplesの時刻0は映像先頭。音声のない部分は0で埋める。
struct NicoJKAlignAudio {
    std::vector<float> samples;
    double firstFrameTime = 0;
    // TOTから映像先頭へ戻す幅を秒に丸めない先頭時刻。評価用(製品はfirstFrameTimeを使う)。
    double firstFrameTimeExact = 0;
    double duration = 0;
    int serviceId = -1;
    bool hasTime = false;
};

NicoJKAlignAudio ReadNicoJKTargetAudio(const tstring& wavePath, const std::vector<FileAudioFrameInfo>& frames,
                                       double videoOriginPTS, double duration);

NicoJKAlignAudio ReadNicoJKReferenceAudio(AMTContext& ctx, const tstring& path, int serviceId = -1);

// 8kHzのfloat(±1の目盛り)を16bit・モノラルのWAVに書く。int16へは丸めて飽和させる。
void WriteNicoJKAlignWav(const tstring& path, const std::vector<float>& samples);

// 未知PTSを無視し、1フレームのL+R平均を8kHzの位置へ置く。
void AddNicoJKAlignAudioFrame(std::vector<float>& samples, int64_t pts, double originPTS, int sampleRate,
                              const int16_t* stereo, size_t count);

double NicoJKAlignPtsDifference(int64_t pts, double origin);
