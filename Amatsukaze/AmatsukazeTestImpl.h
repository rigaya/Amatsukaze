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

namespace test {

int PrintCRCTable(AMTContext& ctx, const ConfigWrapper& setting);

int CheckCRC(AMTContext& ctx, const ConfigWrapper& setting);

int ReadBits(AMTContext& ctx, const ConfigWrapper& setting);

int CheckAutoBuffer(AMTContext& ctx, const ConfigWrapper& setting);

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

int PrintfBug(AMTContext& ctx, const ConfigWrapper& setting);

int ResourceTest(AMTContext& ctx, const ConfigWrapper& setting);

} // namespace test

