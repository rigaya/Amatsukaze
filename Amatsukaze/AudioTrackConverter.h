#pragma once

#include "AudioTrackPlanner.h"
#include "PacketCache.h"

// PCMはWAV順のインターリーブ。5.1はFL, FR, FC, LFE, BL, BR。
std::vector<float> ConvertAudioChannels(const std::vector<float>& pcm, AUDIO_CHANNELS src, AUDIO_CHANNELS dst);
std::vector<std::vector<uint8_t>> ConvertAudioTrackRun(AMTContext& ctx, PacketCache& cache,
    const AudioTrackPlan& plan, size_t begin, size_t end, const std::vector<FileAudioFrameInfo>& frameInfo);

// ネイティブ単体テストからチャンネル変換を呼び出すためのC ABI。
// 成功時0、未対応の組み合わせやPCM長の不正時-1、出力バッファ不足時-2 (outputCountに必要要素数) を返す。
extern "C" AMATSUKAZE_API int ConvertAudioChannelsForTest(const float* input, size_t inputCount, int src, int dst,
    float* output, size_t outputCapacity, size_t* outputCount);
