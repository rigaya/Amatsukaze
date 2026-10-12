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

// ネイティブ単体テスト用: 無音ADTSフレームをframeCount個生成し、同梱FAADとlibavcodecのAAC復号器の両方で復号した結果を返す。
// FAADは16bit整数出力、libavcodecは浮動小数出力で、最大絶対値を求める。
// FAADはモノラルをPS対応のため2chへ展開することがある (チャンネル数は結果に入る)。
// 成功時0、生成・復号の失敗時-1を返す。
struct SilentAdtsDecodeResultForTest {
    int faadChannels;
    long long faadSamples; // 全チャンネル合計のサンプル数
    int faadMaxAbs;
    int avChannels;
    long long avSamples;   // 全チャンネル合計のサンプル数
    double avMaxAbs;
};
extern "C" AMATSUKAZE_API int DecodeSilentAdtsForTest(int layout, int samplingFrequencyIndex, int frameCount,
    SilentAdtsDecodeResultForTest* result);
