#pragma once

#include "StreamUtils.h"

struct FileAudioFrameInfo;

constexpr int AAC_LC_FRAME_SAMPLES = 1024;

enum class AudioTrackOperation { COPY, SILENCE, CONVERT };

struct AudioTrackReference {
    AudioTrackOperation operation = AudioTrackOperation::SILENCE;
    int frameIndex = -1;
    int dualMonoChannel = -1;
    AUDIO_CHANNELS srcLayout = AUDIO_MONO;
    AUDIO_CHANNELS dstLayout = AUDIO_MONO;
};

struct AudioTrackPlan {
    int sourceTrack = 0;
    int logicalTrack = 0;
    AUDIO_CHANNELS layout = AUDIO_MONO;
    int sampleRate = 0;
    int samplingFrequencyIndex = -1;
    tstring name;
    std::vector<AudioTrackReference> frames;
};

int GetAudioSamplingFrequencyIndex(int sampleRate);
int GetAudioAdtsChannelConfiguration(AUDIO_CHANNELS layout);
std::vector<AudioTrackPlan> PlanSeparateAudioTracks(
    const std::vector<std::vector<int>>& input,
    const std::vector<FileAudioFrameInfo>& frameInfo,
    double duration90kHz);
void ValidateAudioTrackPlans(const std::vector<AudioTrackPlan>& plans, double duration90kHz);
