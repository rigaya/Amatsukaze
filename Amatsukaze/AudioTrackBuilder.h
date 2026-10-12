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

// ネイティブ単体テスト用: 1本の入力音声トラックからプランを作り、BuildAudioTrackで各プランをファイルへ書き出して返す。
// 入力フレームはstreamへ連結済みで、frameLengths[i]バイト、frameChannels[i] (AUDIO_CHANNELS、デュアルモノはAUDIO_2LANG) を持つ。
// trackFramesは入力トラックのフレーム番号列 (無音は-1)。mergeが0ならPlanSeparateAudioTracks、それ以外はPlanMergeAudioTracks。
// directoryは書き込み可能な既存ディレクトリで、中間・出力ファイルを作る (削除は呼び出し側で行う)。
// 各プランの書き出しがFormatExceptionで失敗した場合はstatus=1、messageに例外の文面を入れる。
struct BuildAudioTracksInputForTest {
    const uint8_t* stream;
    size_t streamLength;
    const size_t* frameLengths;
    const int* frameChannels;
    size_t frameCount;
    const int* trackFrames;
    size_t trackFrameCount;
    int merge;
    const tchar* directory;
};
constexpr int BUILD_AUDIO_TRACKS_NAME_LENGTH = 32;
constexpr int BUILD_AUDIO_TRACKS_MESSAGE_LENGTH = 96;
struct BuildAudioTracksPlanResultForTest {
    int layout;       // AUDIO_CHANNELS
    int logicalTrack;
    int status;       // 0: 成功、1: FormatExceptionで失敗
    tchar name[BUILD_AUDIO_TRACKS_NAME_LENGTH];
    tchar message[BUILD_AUDIO_TRACKS_MESSAGE_LENGTH];
    size_t outputOffset; // outputバッファ内での書き出し結果の位置
    size_t outputLength;
};
// 成功時0、プラン作成がFormatExceptionで拒否された場合1、引数不正・その他の失敗時-1、
// バッファ不足時-2 (planCount/outputLengthに必要数) を返す。
extern "C" AMATSUKAZE_API int BuildAudioTracksForTest(const BuildAudioTracksInputForTest* input,
    BuildAudioTracksPlanResultForTest* plans, size_t planCapacity, size_t* planCount,
    uint8_t* output, size_t outputCapacity, size_t* outputLength);

// ネイティブ単体テスト用: 合成した映像・音声フレーム情報でStreamReformInfoを作り、音声のプラン作成までの結果を要約する。
// 音声は2本 (トラック0は100フレーム目から欠落を置き300フレーム目で2ch→5.1ch、トラック1は200〜259と400〜449で出現)。
// 判定はテスト側で行う。fallbackが3以上のときは、directoryに無音ADTSの音声ファイルを作る (削除は呼び出し側で行う)。
struct AudioReformScenarioForTest {
    int missingFrames;    // トラック0の100フレーム目から欠落させるフレーム数
    int cut;              // 0以外でCMゾーン (120-220) を落とす
    int sourcePhaseTicks; // トラック0の音声PTSの位相ずれ (90kHz)
    int fallback;         // 0: なし、1: 周波数混在、2: 未対応レイアウト、3: AAC Main、4: PCE (cfg0) ステレオ、5: cfg0デュアルモノ
    int mode;             // AUDIO_FORMAT_CHANGE_MODE
    const tchar* directory;
};
constexpr int AUDIO_REFORM_MAX_PLANS = 8;
struct AudioReformResultForTest {
    int numEncoders;       // getNumEncoders(0)
    int outFileCount;
    int trackCount;        // 出力ファイルの音声トラック数
    int track0Count;       // トラック0の参照列の長さ
    int track1Count;
    int planCount;
    int planFrameCount[AUDIO_REFORM_MAX_PLANS];
    int planHasConvert[AUDIO_REFORM_MAX_PLANS];
    tchar planName[AUDIO_REFORM_MAX_PLANS][16];
    int validatePlans;     // ValidateAudioTrackPlansの結果 (0: 受理、1: FormatException)
    int pcmFrontFrameIndex; // トラック1の先頭の無音参照のPCM入力
    int pcmFrontWaveLength;
    tchar message[BUILD_AUDIO_TRACKS_MESSAGE_LENGTH]; // 例外の文面
};
// track0/track1にはそれぞれ参照列 (元の音声フレーム番号、無音は-1)、track0PtsにはトラックPTS (無音は-1) を返す。
// 成功時0、シナリオ内の例外時1 (resultのmessageに文面)、引数不正時-1、バッファ不足時-2を返す。
extern "C" AMATSUKAZE_API int RunAudioReformScenarioForTest(const AudioReformScenarioForTest* scenario,
    AudioReformResultForTest* result, int* track0, int64_t* track0Pts, int* track1, size_t trackCapacity);
