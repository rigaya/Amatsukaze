#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace trimavs {

// フレーム番号は begin を含み end を含まない半開区間で扱う。
struct FrameRange {
    int begin;
    int end;

    bool operator==(const FrameRange& other) const {
        return begin == other.begin && end == other.end;
    }
};

enum class TrimAvsParseResult {
    Succeeded,
    SyntaxInvalid,
    EmptyAfterNormalize,
};

// AVS の Trim(begin, endInclusive) 列を解析し、正規化済み半開区間へ変換する。
bool ParseTrimAvs(const std::string& text, int numFrames,
    std::vector<FrameRange>& ranges, std::string& error);

// pass0向けに、構文不正と範囲外clamp後の空区間を区別して解析する。
TrimAvsParseResult ParseTrimAvsForPass0(const std::string& text, int numFrames,
    std::vector<FrameRange>& ranges, std::string& error);

// 半開区間を範囲内へ収め、開始順・非重複・非隣接の区間列へ正規化する。
bool NormalizeFrameRanges(const std::vector<FrameRange>& source, int numFrames,
    std::vector<FrameRange>& ranges, std::string& error);

// 既存CM解析の begin/end 交互 vector との変換を行う。
bool FrameRangesFromLegacyTrims(const std::vector<int>& trims, int numFrames,
    std::vector<FrameRange>& ranges, std::string& error);
bool FrameRangesToLegacyTrims(const std::vector<FrameRange>& ranges, int numFrames,
    std::vector<int>& trims, std::string& error);

// 正規化済み半開区間を AVS の閉区間 Trim 列として出力する。
bool FormatTrimAvs(const std::vector<FrameRange>& ranges, int numFrames,
    std::string& text, std::string& error);

// pass0で先頭から採用する本編フレームの計画を作る。rangesは半開区間である。
struct Pass0FramePlan {
    std::vector<FrameRange> decodeRanges;
    int targetFrames = 0;
    int minProgramFrames = 0;
    int programFrames = 0;
    int skippedCmFrames = 0;
};
bool BuildPass0FramePlan(const std::vector<FrameRange>& ranges, int totalFrames, int searchFrames,
    Pass0FramePlan& plan, std::string& error);

// requested順にROIキャッシュへ追加する際、同じresolvedフレームの最初のコピーを参照する。
struct Pass0RoiCacheFrame {
    int cacheIndex = 0;
    int sourceCacheIndex = 0;
    bool firstResolved = false;
};
class Pass0RoiCachePlan {
public:
    Pass0RoiCacheFrame appendResolvedFrame(int resolved);
    int logicalFrameCount() const { return logicalFrameCount_; }

private:
    std::unordered_map<int, int> firstCacheIndexByResolved_;
    int logicalFrameCount_ = 0;
};

} // namespace trimavs
