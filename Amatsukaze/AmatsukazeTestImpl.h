#pragma once

/**
* Amtasukaze Test Implementation
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include "TranscodeManager.h"
#include "LogoScan.h"

// ネイティブ単体テストからコマンドライン引数の解析結果を取得するためのC ABI (定義はAmatsukazeCLI.hpp)。
// 結果は「キー=値」の改行区切りテキスト。成功時0、引数の拒否時1、想定外の例外時2、
// 引数不正時-1、出力バッファ不足時-2 (outputLengthに終端を除く必要文字数) を返す。
extern "C" AMATSUKAZE_API int ParseCommandLineForTest(int argc, const tchar* const* argv,
    tchar* output, size_t outputCapacity, size_t* outputLength);

// ネイティブ単体テスト用: PsStreamWriterでMPEG-PSを書き出し、PsStreamVerifierで検証した結果を返す。
// 各入力は1つのPESパケット (映像はstream_id 0xE0、音声は0xC0) としてペイロードから組み立てる。
// DTSが負の場合はPTSのみのPESにする。出力の末尾にはMPEG_program_end_code (00 00 01 B9) を付ける。
struct PsWriterInputForTest {
    int isVideo;
    int64_t PTS;
    int64_t DTS;
    const uint8_t* payload;
    size_t payloadLength;
};
// 成功時0、PsStreamVerifierの拒否時1、引数不正・その他の失敗時-1、出力バッファ不足時-2 (outputLengthに必要バイト数) を返す。
extern "C" AMATSUKAZE_API int WritePsStreamForTest(const PsWriterInputForTest* inputs, size_t inputCount,
    uint8_t* output, size_t outputCapacity, size_t* outputLength);

namespace test {

int PrintCRCTable(AMTContext& ctx, const ConfigWrapper& setting);

int CheckCRC(AMTContext& ctx, const ConfigWrapper& setting);

int ReadBits(AMTContext& ctx, const ConfigWrapper& setting);

int CheckAutoBuffer(AMTContext& ctx, const ConfigWrapper& setting);

int CaptionTextLength(AMTContext& ctx, const ConfigWrapper& setting);

int VerifyMpeg2Ps(AMTContext& ctx, const ConfigWrapper& setting);

int ReadTS(AMTContext& ctx, const ConfigWrapper& setting);

int AacDecode(AMTContext& ctx, const ConfigWrapper& setting);

int WaveWriteHeader(AMTContext& ctx, const ConfigWrapper& setting);

int ProcessTest(AMTContext& ctx, const ConfigWrapper& setting);

int FileStreamInfo(AMTContext& ctx, const ConfigWrapper& setting);

int ParseArgs(AMTContext& ctx, const ConfigWrapper& setting);

int LogoFrameTest(AMTContext& ctx, const ConfigWrapper& setting);

class TestSplitDualMono : public DualMonoSplitter {
    std::unique_ptr<File> file0;
    std::unique_ptr<File> file1;
public:
    TestSplitDualMono(AMTContext& ctx, const std::vector<tstring>& outpaths);

    virtual void OnOutFrame(int index, MemoryChunk mc);
};

int SplitDualMonoAAC(AMTContext& ctx, const ConfigWrapper& setting);

int AACDecodeTest(AMTContext& ctx, const ConfigWrapper& setting);

int CaptionASS(AMTContext& ctx, const ConfigWrapper& setting);

int EncoderOptionParse(AMTContext& ctx, const ConfigWrapper& setting);

int DecodePerformance(AMTContext& ctx, const ConfigWrapper& setting);

int BitrateZones(AMTContext& ctx, const ConfigWrapper& setting);

int BitrateZonesBug(AMTContext& ctx, const ConfigWrapper& setting);

int VFRInputDetection(AMTContext& ctx, const ConfigWrapper& setting);

int PrintfBug(AMTContext& ctx, const ConfigWrapper& setting);

int ResourceTest(AMTContext& ctx, const ConfigWrapper& setting);

} // namespace test

