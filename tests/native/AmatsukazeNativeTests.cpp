// Amatsukaze の公開ネイティブ単体テスト実行器
#include "CaptionData.h"
#include "FilteredSource.h"
#include "StreamReform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iterator>
#include <string>
#include <vector>

namespace {

bool Expect(bool condition, const char* message, std::string& diagnostic) {
    if (condition) return true;
    diagnostic = message;
    return false;
}

bool ExpectNear(double actual, double expected, double tolerance, const char* name, std::string& diagnostic) {
    if (std::abs(actual - expected) <= tolerance) return true;
    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), "%s: 期待値=%.6f, 実際=%.6f, 許容差=%.6f", name, expected, actual, tolerance);
    diagnostic = buffer;
    return false;
}

bool TestCaptionTextLength(std::string& diagnostic) {
    struct TestCase {
        const wchar_t* text;
        int expected;
    };
    constexpr wchar_t highSurrogate = static_cast<wchar_t>(0xD800);
    constexpr wchar_t lowSurrogate = static_cast<wchar_t>(0xDC00);
    const wchar_t unpairedHighSurrogate[] = { highSurrogate, 0 };
    const wchar_t unpairedLowSurrogate[] = { lowSurrogate, 0 };
    const TestCase testCases[] = {
        { L"", 0 }, { L"AB", 2 }, { L"\U00020000", 1 }, { L"\u9022\uFE00", 1 },
        { L"\u9022\uFE0F", 1 }, { L"\u9022\uFE10", 2 }, { L"\u1820\u180B", 1 },
        { L"\u1820\u180D", 1 }, { L"\u1820\u180F", 1 }, { L"\u9022\U000E0101", 1 },
        { L"\u9022\U000E0100", 1 }, { L"\u9022\U000E01EF", 1 }, { L"\u9022\U000E00FF", 2 },
        { L"\u9022\U000E01F0", 2 }, { L"A\u9022\U000E0101B", 3 }, { L"\U000E0101", 1 },
        { L"\u9022\U000E0101\U000E0102", 2 }, { unpairedHighSurrogate, 1 }, { unpairedLowSurrogate, 1 },
    };
    for (const auto& testCase : testCases) {
        const int actual = CountCaptionTextCharacters(testCase.text);
        if (actual != testCase.expected) {
            char buffer[128] = {};
            std::snprintf(buffer, sizeof(buffer), "字幕文字数: 期待値=%d, 実際=%d", testCase.expected, actual);
            diagnostic = buffer;
            return false;
        }
    }

    struct CharacterTestCase {
        const wchar_t* text;
        bool expectedZeroSpacing;
    };
    const CharacterTestCase characterTestCases[] = {
        { L"A", false }, { L"\u9022", false }, { L"\U00020BB7", true }, { L"\U0001F600", true },
        { L"\u9022\uFE00", true }, { L"\u9022\U000E0101", true }, { L"\u1820\u180B", true }, { L"\U000E0101", true },
    };
    for (const auto& testCase : characterTestCases) {
        const CaptionTextCharacter actual = GetCaptionTextCharacter(testCase.text);
        const size_t expectedLength = std::wstring(testCase.text).size();
        if (actual.length != expectedLength || actual.requiresASSZeroSpacing != testCase.expectedZeroSpacing) {
            char buffer[192] = {};
            std::snprintf(buffer, sizeof(buffer), "字幕文字解析: wchar_t数=%zu/%zu, ASS字間0=%d/%d",
                actual.length, expectedLength, actual.requiresASSZeroSpacing, testCase.expectedZeroSpacing);
            diagnostic = buffer;
            return false;
        }
    }
    return true;
}

bool TestBitrateZones(std::string& diagnostic) {
    const double incompleteTimeCodes[] = { 0.0 };
    size_t invalidZoneCount = 1;
    if (MakeVFRBitrateZonesForTest(incompleteTimeCodes, std::size(incompleteTimeCodes), nullptr, 0,
        0.6, 60000, 1001, 1.0, 0.15, nullptr, 0, &invalidZoneCount) != VFR_BITRATE_ZONES_FOR_TEST_INVALID_ARGUMENT ||
        invalidZoneCount != 0) {
        diagnostic = "終端時刻のないタイムコードを拒否できません";
        return false;
    }
    size_t emptyZoneCount = 0;
    if (MakeVFRBitrateZonesForTest(nullptr, 0, nullptr, 0, 0.6, 60000, 1001, 1.0, 0.15,
        nullptr, 0, &emptyZoneCount) != VFR_BITRATE_ZONES_FOR_TEST_SUCCESS || emptyZoneCount != 0) {
        diagnostic = "フレームを持たないタイムコードは空ゾーンでなければなりません";
        return false;
    }
    std::vector<double> timeCodes;
    double elapsed = 0.0;
    const double tick = 1000.0 * 1001 / 60000;
    for (int i = 0; i < 30; ++i) {
        timeCodes.push_back(elapsed); elapsed += tick * 2;
        timeCodes.push_back(elapsed); elapsed += tick * 3;
    }
    for (int i = 0; i < 40; ++i) {
        timeCodes.push_back(elapsed); elapsed += tick;
    }
    for (int i = 0; i < 50; ++i) {
        timeCodes.push_back(elapsed); elapsed += tick * 2;
    }
    timeCodes.push_back(elapsed);
    const VFRBitrateZoneInputForTest cmzones[] = { { 40, 80 }, { 110, 130 } };
    size_t zoneCount = 0;
    const int countResult = MakeVFRBitrateZonesForTest(timeCodes.data(), timeCodes.size(), cmzones,
        std::size(cmzones), 0.6, 60000, 1001, 1.0, 0.15, nullptr, 0, &zoneCount);
    if (countResult != VFR_BITRATE_ZONES_FOR_TEST_SUCCESS || zoneCount != 4) {
        diagnostic = "ビットレートゾーン数が一致しません: 期待値=4, 実際=" + std::to_string(zoneCount);
        return false;
    }
    std::vector<VFRBitrateZoneOutputForTest> smallBuffer(zoneCount - 1, { -1, -1, -1.0, -1.0, -1.0, -1.0 });
    size_t requiredZoneCount = 0;
    const int smallBufferResult = MakeVFRBitrateZonesForTest(timeCodes.data(), timeCodes.size(), cmzones,
        std::size(cmzones), 0.6, 60000, 1001, 1.0, 0.15, smallBuffer.data(), smallBuffer.size(), &requiredZoneCount);
    if (smallBufferResult != VFR_BITRATE_ZONES_FOR_TEST_BUFFER_TOO_SMALL || requiredZoneCount != zoneCount ||
        smallBuffer[0].startFrame != -1) {
        diagnostic = "ビットレートゾーンの小さい出力バッファを正しく処理できません";
        return false;
    }
    std::vector<VFRBitrateZoneOutputForTest> zones(zoneCount);
    const int copyResult = MakeVFRBitrateZonesForTest(timeCodes.data(), timeCodes.size(), cmzones,
        std::size(cmzones), 0.6, 60000, 1001, 1.0, 0.15, zones.data(), zones.size(), &zoneCount);
    if (copyResult != VFR_BITRATE_ZONES_FOR_TEST_SUCCESS || zoneCount != zones.size()) {
        diagnostic = "ビットレートゾーンを出力バッファへコピーできません";
        return false;
    }
    if (zones.size() != 4) {
        diagnostic = "ビットレートゾーン数が一致しません: 期待値=4, 実際=" + std::to_string(zones.size());
        for (const auto& zone : zones) {
            char buffer[128] = {};
            std::snprintf(buffer, sizeof(buffer), " [%d-%d, %.6f]", zone.startFrame, zone.endFrame, zone.bitrate);
            diagnostic += buffer;
        }
        return false;
    }
    // 8フレーム単位の入力から得る4ゾーンを固定する。40-64は(1.5+1.5+1.05)/3=1.35、
    // 64-128は(0.6+0.6+1+1+1.5+2+1.2+1.2)/8=1.1375となる。
    // 19単位の上限は19*0.15=2.85である。5ゾーン時の累積コスト2.366666...に最小併合コスト0.8が加わり、
    // 4ゾーン時は3.166666...になる。次の反復は上限超過で停止するため、4→3の併合コスト0.927272...は適用されない。
    if (!Expect(zones[0].startFrame == 0 && zones[0].endFrame == 40, "先頭ゾーンのフレーム範囲が一致しません", diagnostic)) return false;
    if (!ExpectNear(zones[0].bitrate, 2.5, 1e-9, "先頭ゾーンのビットレート", diagnostic)) return false;
    if (!Expect(zones[1].startFrame == 40 && zones[1].endFrame == 64, "第2ゾーンのフレーム範囲が一致しません", diagnostic)) return false;
    if (!ExpectNear(zones[1].bitrate, 1.35, 1e-9, "第2ゾーンのビットレート", diagnostic)) return false;
    if (!Expect(zones[2].startFrame == 64 && zones[2].endFrame == 128, "第3ゾーンのフレーム範囲が一致しません", diagnostic)) return false;
    if (!ExpectNear(zones[2].bitrate, 1.1375, 1e-9, "第3ゾーンのビットレート", diagnostic)) return false;
    if (!Expect(zones[3].startFrame == 128 && zones[3].endFrame == 150, "末尾ゾーンのフレーム範囲が一致しません", diagnostic)) return false;
    return ExpectNear(zones[3].bitrate, 2.0, 1e-9, "末尾ゾーンのビットレート", diagnostic);
}

std::vector<VFRFrameInterval> MakeIntervals(int normalCount, int alternateCount, double normal, double alternate,
    double normalRepeat = 1.0, double alternateRepeat = 1.0) {
    std::vector<VFRFrameInterval> intervals;
    intervals.reserve(normalCount + alternateCount);
    for (int i = 0; i < normalCount; ++i) intervals.push_back({ normal, normalRepeat });
    for (int i = 0; i < alternateCount; ++i) intervals.push_back({ alternate, alternateRepeat });
    return intervals;
}

bool AnalyzeVFRIntervalsForTest(const std::vector<VFRFrameInterval>& intervals,
    VFRDetectionResult& result, std::string& diagnostic) {
    const int status = AnalyzeVFRFrameIntervalsForTest(intervals.data(), intervals.size(), &result);
    if (status == VFR_INPUT_DETECTION_FOR_TEST_SUCCESS) return true;
    diagnostic = "VFR入力判定を実行できません";
    return false;
}

bool TestVFRInputDetection(std::string& diagnostic) {
    const struct TestCase {
        const char* name;
        std::vector<VFRFrameInterval> intervals;
        bool expected;
    } testCases[] = {
        { "CFR", MakeIntervals(1000, 0, 3003.0, 0.0), false },
        { "微小なPTS差", MakeIntervals(500, 500, 3000.0, 3003.0), false },
        { "RFF", MakeIntervals(750, 250, 3003.0, 4504.5, 1.0, 1.5), false },
        { "少数drop", MakeIntervals(950, 50, 3003.0, 6006.0), false },
        { "RFF直後の少数drop", MakeIntervals(950, 50, 3003.0, 7507.5, 1.0, 1.5), false },
        { "VFR", MakeIntervals(750, 250, 3003.0, 4500.0), true },
        { "継続的なPTS間隔異常", MakeIntervals(750, 250, 3003.0, 9009.0), true },
        { "10%未満の副候補", MakeIntervals(950, 50, 3003.0, 4500.0), false },
    };
    for (const auto& testCase : testCases) {
        VFRDetectionResult result = {};
        if (!AnalyzeVFRIntervalsForTest(testCase.intervals, result, diagnostic)) return false;
        const bool actual = result.detected;
        if (actual != testCase.expected) {
            diagnostic = std::string("VFR判定が一致しません: ") + testCase.name;
            return false;
        }
    }
    auto mixedIntervals = MakeIntervals(600, 200, 1501.5, 3753.75);
    const auto fps120 = MakeIntervals(0, 200, 0.0, 750.75);
    mixedIntervals.insert(mixedIntervals.end(), fps120.begin(), fps120.end());
    VFRDetectionResult result = {};
    if (!AnalyzeVFRIntervalsForTest(mixedIntervals, result, diagnostic)) return false;
    return Expect(result.detected,
        "24/60/120fps混在をVFRとして検出できませんでした", diagnostic);
}

struct TestCase {
    const char* name;
    bool (*run)(std::string& diagnostic);
};

constexpr TestCase TEST_CASES[] = {
    { "caption_text_length", TestCaptionTextLength },
    { "bitrate_zones", TestBitrateZones },
    { "vfr_input_detection", TestVFRInputDetection },
};

void PrintUsage(const char* program) {
    std::printf("使用法: %s [--list] [--select <ケース名>]...\n", program);
}

bool IsSelected(const TestCase& testCase, const std::vector<std::string>& selectedNames) {
    return selectedNames.empty()
        || std::find(selectedNames.begin(), selectedNames.end(), testCase.name) != selectedNames.end();
}

bool IsRegisteredName(const std::string& name) {
    return std::any_of(std::begin(TEST_CASES), std::end(TEST_CASES), [&name](const TestCase& testCase) {
        return name == testCase.name;
    });
}

int RunTestCase(const TestCase& testCase) {
    try {
        std::string diagnostic;
        if (testCase.run(diagnostic)) {
            std::printf("[PASS] %s\n", testCase.name);
            return 0;
        }
        std::fprintf(stderr, "[FAIL] %s: %s\n", testCase.name, diagnostic.c_str());
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[FAIL] %s: 例外: %s\n", testCase.name, exception.what());
    } catch (...) {
        std::fprintf(stderr, "[FAIL] %s: 不明な例外です。\n", testCase.name);
    }
    return 1;
}

}

int main(int argc, char* argv[]) {
    bool listOnly = false;
    std::vector<std::string> selectedNames;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--list") {
            listOnly = true;
        } else if (argument == "--select") {
            if (++i >= argc) {
                std::fprintf(stderr, "--select にはケース名が必要です。\n");
                PrintUsage(argv[0]);
                return 2;
            }
            selectedNames.emplace_back(argv[i]);
        } else if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "不明な引数です: %s\n", argument.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }
    for (const auto& name : selectedNames) {
        if (!IsRegisteredName(name)) {
            std::fprintf(stderr, "未登録のケースです: %s。--list で確認してください。\n", name.c_str());
            return 2;
        }
    }
    if (listOnly) {
        for (const auto& testCase : TEST_CASES) std::puts(testCase.name);
        return 0;
    }

    int selectedCount = 0;
    int failureCount = 0;
    for (const auto& testCase : TEST_CASES) {
        if (!IsSelected(testCase, selectedNames)) continue;
        ++selectedCount;
        failureCount += RunTestCase(testCase);
    }
    if (selectedCount == 0) {
        std::fprintf(stderr, "選択したケースは登録されていません。--list で確認してください。\n");
        return 2;
    }
    std::printf("実行: %d件, 失敗: %d件\n", selectedCount, failureCount);
    return failureCount == 0 ? 0 : 1;
}
