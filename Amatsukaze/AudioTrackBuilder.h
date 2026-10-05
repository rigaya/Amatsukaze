#pragma once

#include "AudioTrackPlanner.h"
#include "StreamReform.h"
#include "PacketCache.h"

std::vector<uint8_t> GenerateSilentAdtsFrame(AUDIO_CHANNELS layout, int samplingFrequencyIndex);

// ネイティブ単体テストから無音ADTSフレーム生成を呼び出すためのC ABI。
// 成功時0、引数不正/未対応フォーマット時-1、出力バッファ不足時-2 (frameLengthに必要バイト数) を返す。
extern "C" AMATSUKAZE_API int GenerateSilentAdtsFrameForTest(int layout, int samplingFrequencyIndex,
    uint8_t* output, size_t outputCapacity, size_t* frameLength);
void BuildAudioTrack(AMTContext& ctx, PacketCache& cache, const AudioTrackPlan& plan, const tstring& path,
    const std::vector<FileAudioFrameInfo>& frameInfo = {});
