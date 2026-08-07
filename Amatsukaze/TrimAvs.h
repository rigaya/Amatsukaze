#pragma once

#include <string>
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

// AVS の Trim(begin, endInclusive) 列を解析し、正規化済み半開区間へ変換する。
bool ParseTrimAvs(const std::string& text, int numFrames,
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

} // namespace trimavs
