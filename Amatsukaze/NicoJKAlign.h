#pragma once
#include <string>
#include <vector>

// 時刻は対象・基準それぞれの録画先頭からの秒。deltaは基準－対象。
struct NicoJKAlignSegment {
    double start = 0;
    double end = 0;
    double delta = 0;
    int probes = 0;
    double ratio = 0;
};
struct NicoJKTimeMapEntry {
    double referenceStart = 0;
    double referenceEnd = 0;
    double targetStart = 0;
    double targetEnd = 0;
};
struct NicoJKAlignResult {
    bool success = false;
    std::string reason;
    std::vector<NicoJKAlignSegment> segments;
    double seconds = 0;
};

NicoJKAlignResult AlignNicoJKAudio(const std::vector<float>& target8k, const std::vector<float>& reference8k);
std::vector<NicoJKTimeMapEntry> MakeNicoJKTimeMap(const std::vector<NicoJKAlignSegment>& segments,
                                                  double targetDuration, double referenceDuration);
std::string NicoJKTimeMapJSON(const std::vector<NicoJKTimeMapEntry>& map);
std::string NicoJKAlignResultJSON(const NicoJKAlignResult& result, double targetDuration, double referenceDuration);
