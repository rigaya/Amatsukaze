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
std::vector<AudioTrackPlan> PlanMergeAudioTracks(
    const std::vector<std::vector<int>>& input,
    const std::vector<FileAudioFrameInfo>& frameInfo,
    double duration90kHz);
void ValidateAudioTrackPlans(const std::vector<AudioTrackPlan>& plans, double duration90kHz);

// ネイティブ単体テストから音声トラックプランを検証するためのC ABI。
// STLコンテナをDLL境界で渡さないよう、入力は平坦な配列、出力は呼出側バッファで受け渡す。
// フレーム参照は全プラン分を先頭から順に並べ、各プランのframeCount個ずつがそのプランに属する。
constexpr int AUDIO_TRACK_PLAN_FOR_TEST_SUCCESS = 0;
constexpr int AUDIO_TRACK_PLAN_FOR_TEST_REJECTED = 1;          // FormatExceptionによる拒否
constexpr int AUDIO_TRACK_PLAN_FOR_TEST_INVALID_ARGUMENT = -1;
constexpr int AUDIO_TRACK_PLAN_FOR_TEST_BUFFER_TOO_SMALL = -2; // planCount/refCountに必要数を返す
constexpr int AUDIO_TRACK_PLAN_FOR_TEST_NAME_LENGTH = 32;

struct AudioTrackFrameInfoForTest {
    int channels; // AUDIO_CHANNELS
    int sampleRate;
    int numSamples;
};

struct AudioTrackReferenceForTest {
    int operation; // AudioTrackOperation
    int frameIndex;
    int dualMonoChannel;
    int srcLayout; // AUDIO_CHANNELS
    int dstLayout; // AUDIO_CHANNELS
};

struct AudioTrackPlanForTest {
    int sourceTrack;
    int logicalTrack;
    int layout; // AUDIO_CHANNELS
    int sampleRate;
    int samplingFrequencyIndex;
    tchar name[AUDIO_TRACK_PLAN_FOR_TEST_NAME_LENGTH];
    size_t frameCount;
};

// merge != 0 でPlanMergeAudioTracks、0でPlanSeparateAudioTracksを呼ぶ。
// 入力トラックは trackLengths[i] 個のフレーム参照を frameIndices に連続して並べる。
extern "C" AMATSUKAZE_API int PlanAudioTracksForTest(int merge,
    const int* frameIndices, const size_t* trackLengths, size_t trackCount,
    const AudioTrackFrameInfoForTest* frameInfo, size_t frameInfoCount, double duration90kHz,
    AudioTrackPlanForTest* plans, size_t planCapacity, size_t* planCount,
    AudioTrackReferenceForTest* references, size_t referenceCapacity, size_t* referenceCount);

// ValidateAudioTrackPlansを呼ぶ。references は全プランのframeCountの合計個数が必要。
extern "C" AMATSUKAZE_API int ValidateAudioTrackPlansForTest(
    const AudioTrackPlanForTest* plans, size_t planCount,
    const AudioTrackReferenceForTest* references, double duration90kHz);
