#include "TrimAvs.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <regex>
#include <sstream>

namespace trimavs {
namespace {

bool SetError(std::string& error, const char* message) {
    error = message;
    return false;
}

int ClampFrame(const int64_t value, const int numFrames) {
    if (value <= 0) {
        return 0;
    }
    if (value >= numFrames) {
        return numFrames;
    }
    return static_cast<int>(value);
}

bool ParseInt64(const std::string& text, int64_t& value) {
    if (text.empty()) {
        return false;
    }

    size_t pos = 0;
    bool negative = false;
    if (text[pos] == '+' || text[pos] == '-') {
        negative = text[pos] == '-';
        ++pos;
    }
    if (pos == text.size()) {
        return false;
    }

    const uint64_t limit = negative
        ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u
        : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t magnitude = 0;
    for (; pos < text.size(); ++pos) {
        const unsigned char ch = static_cast<unsigned char>(text[pos]);
        if (!std::isdigit(ch)) {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (magnitude > (limit - digit) / 10u) {
            return false;
        }
        magnitude = magnitude * 10u + digit;
    }

    if (negative) {
        if (magnitude == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u) {
            value = std::numeric_limits<int64_t>::min();
        } else {
            value = -static_cast<int64_t>(magnitude);
        }
    } else {
        value = static_cast<int64_t>(magnitude);
    }
    return true;
}

bool HasInvalidTrimCall(const std::string& text,
    const std::vector<std::pair<size_t, size_t>>& validCalls) {
    static const std::regex trimStart("\\btrim\\s*\\(", std::regex::icase);
    for (std::sregex_iterator it(text.begin(), text.end(), trimStart), end; it != end; ++it) {
        const size_t pos = static_cast<size_t>(it->position());
        const bool found = std::any_of(validCalls.begin(), validCalls.end(),
            [pos](const std::pair<size_t, size_t>& call) {
                return pos >= call.first && pos < call.second;
            });
        if (!found) {
            return true;
        }
    }
    return false;
}

} // namespace

bool NormalizeFrameRanges(const std::vector<FrameRange>& source, const int numFrames,
    std::vector<FrameRange>& ranges, std::string& error) {
    ranges.clear();
    error.clear();
    if (numFrames <= 0) {
        return SetError(error, "総フレーム数が不正です");
    }

    for (const auto& range : source) {
        if (range.begin > range.end) {
            return SetError(error, "Trimの開始フレームが終了フレームより後です");
        }
        const int begin = ClampFrame(range.begin, numFrames);
        const int end = ClampFrame(range.end, numFrames);
        if (begin < end) {
            ranges.push_back({ begin, end });
        }
    }

    std::sort(ranges.begin(), ranges.end(), [](const FrameRange& lhs, const FrameRange& rhs) {
        return lhs.begin != rhs.begin ? lhs.begin < rhs.begin : lhs.end < rhs.end;
    });

    std::vector<FrameRange> merged;
    for (const auto& range : ranges) {
        if (merged.empty() || merged.back().end < range.begin) {
            merged.push_back(range);
        } else {
            merged.back().end = std::max(merged.back().end, range.end);
        }
    }
    ranges = std::move(merged);
    if (ranges.empty()) {
        return SetError(error, "正規化後のTrim区間が空です");
    }
    return true;
}

bool ParseTrimAvs(const std::string& text, const int numFrames,
    std::vector<FrameRange>& ranges, std::string& error) {
    ranges.clear();
    error.clear();
    if (numFrames <= 0) {
        return SetError(error, "総フレーム数が不正です");
    }

    static const std::regex trimCall(
        "\\btrim\\s*\\(\\s*([+-]?\\d+)\\s*,\\s*([+-]?\\d+)\\s*\\)", std::regex::icase);
    std::vector<FrameRange> parsed;
    std::vector<std::pair<size_t, size_t>> validCalls;
    for (std::sregex_iterator it(text.begin(), text.end(), trimCall), end; it != end; ++it) {
        const std::smatch& match = *it;
        int64_t begin = 0;
        int64_t endInclusive = 0;
        if (!ParseInt64(match[1].str(), begin) || !ParseInt64(match[2].str(), endInclusive)) {
            return SetError(error, "Trimのフレーム番号が整数範囲を超えています");
        }
        if (begin > endInclusive) {
            return SetError(error, "Trimの開始フレームが終了フレームより後です");
        }

        // endInclusive + 1 は int64_t のまま加算せず、先に出力範囲へ丸める。
        const int clampedBegin = ClampFrame(begin, numFrames);
        const int clampedEnd = endInclusive < 0 ? 0
            : endInclusive >= static_cast<int64_t>(numFrames) - 1 ? numFrames
            : static_cast<int>(endInclusive + 1);
        parsed.push_back({ clampedBegin, clampedEnd });
        const size_t beginPos = static_cast<size_t>(match.position());
        validCalls.emplace_back(beginPos, beginPos + static_cast<size_t>(match.length()));
    }

    if (HasInvalidTrimCall(text, validCalls)) {
        return SetError(error, "Trim呼び出しの構文または引数が不正です");
    }
    if (parsed.empty()) {
        return SetError(error, "Trim呼び出しがありません");
    }
    return NormalizeFrameRanges(parsed, numFrames, ranges, error);
}

bool FrameRangesFromLegacyTrims(const std::vector<int>& trims, const int numFrames,
    std::vector<FrameRange>& ranges, std::string& error) {
    ranges.clear();
    error.clear();
    if (trims.size() % 2 != 0) {
        return SetError(error, "旧形式Trim区間数が不正です");
    }

    std::vector<FrameRange> source;
    source.reserve(trims.size() / 2);
    for (size_t i = 0; i < trims.size(); i += 2) {
        source.push_back({ trims[i], trims[i + 1] });
    }
    return NormalizeFrameRanges(source, numFrames, ranges, error);
}

bool FrameRangesToLegacyTrims(const std::vector<FrameRange>& ranges, const int numFrames,
    std::vector<int>& trims, std::string& error) {
    trims.clear();
    std::vector<FrameRange> normalized;
    if (!NormalizeFrameRanges(ranges, numFrames, normalized, error)) {
        return false;
    }
    trims.reserve(normalized.size() * 2);
    for (const auto& range : normalized) {
        trims.push_back(range.begin);
        trims.push_back(range.end);
    }
    return true;
}

bool FormatTrimAvs(const std::vector<FrameRange>& ranges, const int numFrames,
    std::string& text, std::string& error) {
    text.clear();
    std::vector<FrameRange> normalized;
    if (!NormalizeFrameRanges(ranges, numFrames, normalized, error)) {
        return false;
    }

    std::ostringstream output;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (i > 0) {
            output << " ++ ";
        }
        output << "Trim(" << normalized[i].begin << ", " << normalized[i].end - 1 << ")";
    }
    text = output.str();
    error.clear();
    return true;
}

} // namespace trimavs
