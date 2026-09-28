#pragma once

#include "AudioTrackPlanner.h"
#include "PacketCache.h"

// PCMはWAV順のインターリーブ。5.1はFL, FR, FC, LFE, BL, BR。
std::vector<float> ConvertAudioChannels(const std::vector<float>& pcm, AUDIO_CHANNELS src, AUDIO_CHANNELS dst);
std::vector<std::vector<uint8_t>> ConvertAudioTrackRun(AMTContext& ctx, PacketCache& cache,
    const AudioTrackPlan& plan, size_t begin, size_t end, const std::vector<FileAudioFrameInfo>& frameInfo);
