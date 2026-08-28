#pragma once

#include <cstdint>
#include <vector>

#include "StreamUtils.h"

class ConfigWrapper;
class StreamReformInfo;

enum class Mpeg2PartialAction {
    COPY,
    DROP,
};

struct Mpeg2PartialPatchRange {
    int first;
    int last; // 半開区間
};

struct Mpeg2PartialEncodePlan {
    int dtsFrameStart = 0;
    std::vector<Mpeg2PartialAction> actions; // 中間映像ファイル内のDTS順
    std::vector<int64_t> pts90k;
    std::vector<int64_t> dts90k;
    std::vector<Mpeg2PartialPatchRange> patches;
};

bool BuildMpeg2PartialEncodePlan(
    const StreamReformInfo& reformInfo,
    EncodeFileKey key,
    Mpeg2PartialEncodePlan& plan,
    tstring& reason);

// フェーズ2ではpatch空のときだけMPEG-TSを構築する。
bool TryMpeg2PartialEncode(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    EncodeFileKey key,
    tstring& reason);
