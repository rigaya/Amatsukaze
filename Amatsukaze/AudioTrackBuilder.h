#pragma once

#include "AudioTrackPlanner.h"
#include "PacketCache.h"

std::vector<uint8_t> GenerateSilentAdtsFrame(AUDIO_CHANNELS layout, int samplingFrequencyIndex);
void BuildAudioTrack(AMTContext& ctx, PacketCache& cache, const AudioTrackPlan& plan, const tstring& path);
