#pragma once

#include <cstdint>
#include <vector>

#include "StreamUtils.h"

class ConfigWrapper;
class StreamReformInfo;

enum class Mpeg2PartialAction {
    COPY,
    DROP,
    PATCH,
};

struct Mpeg2PartialPatchRange {
    int first;
    int last; // 半開区間
};

struct Mpeg2PartialOutputEntry {
    Mpeg2PartialAction kind = Mpeg2PartialAction::DROP;
    int localDts = -1;
    int patchIndex = -1;
    int patchPicture = -1;
    int64_t pts90k = 0;
    int64_t dts90k = 0;
};

struct Mpeg2PartialEncodePlan {
    int dtsFrameStart = 0;
    std::vector<Mpeg2PartialAction> actions; // 中間映像ファイル内のDTS順
    std::vector<Mpeg2PartialPatchRange> patches;
    std::vector<Mpeg2PartialOutputEntry> outputEntries; // 出力符号化順
};

bool BuildMpeg2PartialEncodePlan(
    const StreamReformInfo& reformInfo,
    EncodeFileKey key,
    Mpeg2PartialEncodePlan& plan,
    tstring& reason);

bool TryMpeg2PartialEncode(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    EncodeFileKey key,
    tstring& reason);
