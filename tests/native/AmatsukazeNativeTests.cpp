// Amatsukaze の公開ネイティブ単体テスト実行器
#include "AMTLogo.h"
#include "LogoScan.h"
#include "LogoScanPartition.h"
#include "LogoResidualWeight.h"
#include "LogoRoiCache.h"
#include "LogoTemporalHistogram.h"
#include "AdtsParser.h"
#include "AmatsukazeTestImpl.h"
#include "AudioTrackBuilder.h"
#include "AudioTrackConverter.h"
#include "AudioTrackPlanner.h"
#include "CaptionData.h"
#include "CaptionPgs.h"
#include "EncoderOptionParser.h"
#include "FilteredSource.h"
#include "Mpeg2TsParser.h"
#include "StreamReform.h"
#include "StreamUtils.h"
#include "TsInfo.h"
#include "TsSplitter.h"
#include "WaveWriter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <clocale>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

bool Expect(bool condition, const TCHAR* message, tstring& diagnostic) {
    if (condition) return true;
    diagnostic = message;
    return false;
}

bool ExpectNear(double actual, double expected, double tolerance, const TCHAR* name, tstring& diagnostic) {
    if (std::abs(actual - expected) <= tolerance) return true;
    diagnostic = strsprintf(_T("%s: 期待値=%.6f, 実際=%.6f, 許容差=%.6f"), name, expected, actual, tolerance);
    return false;
}

bool TestCaptionTextLength(tstring& diagnostic) {
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
            diagnostic = strsprintf(_T("字幕文字数: 期待値=%d, 実際=%d"), testCase.expected, actual);
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
            diagnostic = strsprintf(_T("字幕文字解析: wchar_t数=%zu/%zu, ASS字間0=%d/%d"),
                actual.length, expectedLength, actual.requiresASSZeroSpacing, testCase.expectedZeroSpacing);
            return false;
        }
    }
    return true;
}

bool TestBitrateZones(tstring& diagnostic) {
    const double incompleteTimeCodes[] = { 0.0 };
    size_t invalidZoneCount = 1;
    if (MakeVFRBitrateZonesForTest(incompleteTimeCodes, std::size(incompleteTimeCodes), nullptr, 0,
        0.6, 60000, 1001, 1.0, 0.15, nullptr, 0, &invalidZoneCount) != VFR_BITRATE_ZONES_FOR_TEST_INVALID_ARGUMENT ||
        invalidZoneCount != 0) {
        diagnostic = _T("終端時刻のないタイムコードを拒否できません");
        return false;
    }
    size_t emptyZoneCount = 0;
    if (MakeVFRBitrateZonesForTest(nullptr, 0, nullptr, 0, 0.6, 60000, 1001, 1.0, 0.15,
        nullptr, 0, &emptyZoneCount) != VFR_BITRATE_ZONES_FOR_TEST_SUCCESS || emptyZoneCount != 0) {
        diagnostic = _T("フレームを持たないタイムコードは空ゾーンでなければなりません");
        return false;
    }
    std::vector<double> timeCodes;
    double elapsed = 0.0;
    const double tick = 1000.0 * 1001 / 60000;
    for (int i = 0; i < 30; i++) {
        timeCodes.push_back(elapsed); elapsed += tick * 2;
        timeCodes.push_back(elapsed); elapsed += tick * 3;
    }
    for (int i = 0; i < 40; i++) {
        timeCodes.push_back(elapsed); elapsed += tick;
    }
    for (int i = 0; i < 50; i++) {
        timeCodes.push_back(elapsed); elapsed += tick * 2;
    }
    timeCodes.push_back(elapsed);
    const VFRBitrateZoneInputForTest cmzones[] = { { 40, 80 }, { 110, 130 } };
    size_t zoneCount = 0;
    const int countResult = MakeVFRBitrateZonesForTest(timeCodes.data(), timeCodes.size(), cmzones,
        std::size(cmzones), 0.6, 60000, 1001, 1.0, 0.15, nullptr, 0, &zoneCount);
    if (countResult != VFR_BITRATE_ZONES_FOR_TEST_SUCCESS || zoneCount != 4) {
        diagnostic = strsprintf(_T("ビットレートゾーン数が一致しません: 期待値=4, 実際=%zu"), zoneCount);
        return false;
    }
    std::vector<VFRBitrateZoneOutputForTest> smallBuffer(zoneCount - 1, { -1, -1, -1.0, -1.0, -1.0, -1.0 });
    size_t requiredZoneCount = 0;
    const int smallBufferResult = MakeVFRBitrateZonesForTest(timeCodes.data(), timeCodes.size(), cmzones,
        std::size(cmzones), 0.6, 60000, 1001, 1.0, 0.15, smallBuffer.data(), smallBuffer.size(), &requiredZoneCount);
    if (smallBufferResult != VFR_BITRATE_ZONES_FOR_TEST_BUFFER_TOO_SMALL || requiredZoneCount != zoneCount ||
        smallBuffer[0].startFrame != -1) {
        diagnostic = _T("ビットレートゾーンの小さい出力バッファを正しく処理できません");
        return false;
    }
    std::vector<VFRBitrateZoneOutputForTest> zones(zoneCount);
    const int copyResult = MakeVFRBitrateZonesForTest(timeCodes.data(), timeCodes.size(), cmzones,
        std::size(cmzones), 0.6, 60000, 1001, 1.0, 0.15, zones.data(), zones.size(), &zoneCount);
    if (copyResult != VFR_BITRATE_ZONES_FOR_TEST_SUCCESS || zoneCount != zones.size()) {
        diagnostic = _T("ビットレートゾーンを出力バッファへコピーできません");
        return false;
    }
    if (zones.size() != 4) {
        diagnostic = strsprintf(_T("ビットレートゾーン数が一致しません: 期待値=4, 実際=%zu"), zones.size());
        for (const auto& zone : zones) {
            diagnostic += strsprintf(_T(" [%d-%d, %.6f]"), zone.startFrame, zone.endFrame, zone.bitrate);
        }
        return false;
    }
    // 8フレーム単位の入力から得る4ゾーンを固定する。40-64は(1.5+1.5+1.05)/3=1.35、
    // 64-128は(0.6+0.6+1+1+1.5+2+1.2+1.2)/8=1.1375となる。
    // 19単位の上限は19*0.15=2.85である。5ゾーン時の累積コスト2.366666...に最小併合コスト0.8が加わり、
    // 4ゾーン時は3.166666...になる。次の反復は上限超過で停止するため、4→3の併合コスト0.927272...は適用されない。
    if (!Expect(zones[0].startFrame == 0 && zones[0].endFrame == 40, _T("先頭ゾーンのフレーム範囲が一致しません"), diagnostic)) return false;
    if (!ExpectNear(zones[0].bitrate, 2.5, 1e-9, _T("先頭ゾーンのビットレート"), diagnostic)) return false;
    if (!Expect(zones[1].startFrame == 40 && zones[1].endFrame == 64, _T("第2ゾーンのフレーム範囲が一致しません"), diagnostic)) return false;
    if (!ExpectNear(zones[1].bitrate, 1.35, 1e-9, _T("第2ゾーンのビットレート"), diagnostic)) return false;
    if (!Expect(zones[2].startFrame == 64 && zones[2].endFrame == 128, _T("第3ゾーンのフレーム範囲が一致しません"), diagnostic)) return false;
    if (!ExpectNear(zones[2].bitrate, 1.1375, 1e-9, _T("第3ゾーンのビットレート"), diagnostic)) return false;
    if (!Expect(zones[3].startFrame == 128 && zones[3].endFrame == 150, _T("末尾ゾーンのフレーム範囲が一致しません"), diagnostic)) return false;
    return ExpectNear(zones[3].bitrate, 2.0, 1e-9, _T("末尾ゾーンのビットレート"), diagnostic);
}

std::vector<VFRFrameInterval> MakeIntervals(int normalCount, int alternateCount, double normal, double alternate,
    double normalRepeat = 1.0, double alternateRepeat = 1.0) {
    std::vector<VFRFrameInterval> intervals;
    intervals.reserve(normalCount + alternateCount);
    for (int i = 0; i < normalCount; i++) intervals.push_back({ normal, normalRepeat });
    for (int i = 0; i < alternateCount; i++) intervals.push_back({ alternate, alternateRepeat });
    return intervals;
}

bool AnalyzeVFRIntervalsForTest(const std::vector<VFRFrameInterval>& intervals,
    VFRDetectionResult& result, tstring& diagnostic) {
    const int status = AnalyzeVFRFrameIntervalsForTest(intervals.data(), intervals.size(), &result);
    if (status == VFR_INPUT_DETECTION_FOR_TEST_SUCCESS) return true;
    diagnostic = _T("VFR入力判定を実行できません");
    return false;
}

bool TestVFRInputDetection(tstring& diagnostic) {
    const struct TestCase {
        const TCHAR* name;
        std::vector<VFRFrameInterval> intervals;
        bool expected;
    } testCases[] = {
        { _T("CFR"), MakeIntervals(1000, 0, 3003.0, 0.0), false },
        { _T("微小なPTS差"), MakeIntervals(500, 500, 3000.0, 3003.0), false },
        { _T("RFF"), MakeIntervals(750, 250, 3003.0, 4504.5, 1.0, 1.5), false },
        { _T("少数drop"), MakeIntervals(950, 50, 3003.0, 6006.0), false },
        { _T("RFF直後の少数drop"), MakeIntervals(950, 50, 3003.0, 7507.5, 1.0, 1.5), false },
        { _T("VFR"), MakeIntervals(750, 250, 3003.0, 4500.0), true },
        { _T("継続的なPTS間隔異常"), MakeIntervals(750, 250, 3003.0, 9009.0), true },
        { _T("10%未満の副候補"), MakeIntervals(950, 50, 3003.0, 4500.0), false },
    };
    for (const auto& testCase : testCases) {
        VFRDetectionResult result = {};
        if (!AnalyzeVFRIntervalsForTest(testCase.intervals, result, diagnostic)) return false;
        const bool actual = result.detected;
        if (actual != testCase.expected) {
            diagnostic = tstring(_T("VFR判定が一致しません: ")) + testCase.name;
            return false;
        }
    }
    auto mixedIntervals = MakeIntervals(600, 200, 1501.5, 3753.75);
    const auto fps120 = MakeIntervals(0, 200, 0.0, 750.75);
    mixedIntervals.insert(mixedIntervals.end(), fps120.begin(), fps120.end());
    VFRDetectionResult result = {};
    if (!AnalyzeVFRIntervalsForTest(mixedIntervals, result, diagnostic)) return false;
    return Expect(result.detected,
        _T("24/60/120fps混在をVFRとして検出できませんでした"), diagnostic);
}

bool ExpectHex(uint32_t actual, uint32_t expected, const TCHAR* name, tstring& diagnostic) {
    if (actual == expected) return true;
    diagnostic = strsprintf(_T("%s: 期待値=0x%08X, 実際=0x%08X"), name, expected, actual);
    return false;
}

bool TestCRC32(tstring& diagnostic) {
    const CRC32 crc;
    // MPEG-2 PSIで使うCRC-32 (多項式0x04C11DB7, 非反転, 初期値0xFFFFFFFF, 最終XORなし)
    constexpr uint32_t CRC_INITIAL = 0xFFFFFFFFu;
    const uint32_t* table = crc.getTable();
    if (!ExpectHex(table[0], 0x00000000u, _T("CRCテーブル[0]"), diagnostic)) return false;
    if (!ExpectHex(table[1], 0x04C11DB7u, _T("CRCテーブル[1]"), diagnostic)) return false;
    if (!ExpectHex(table[255], 0xB1F740B4u, _T("CRCテーブル[255]"), diagnostic)) return false;

    const uint8_t checkInput[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    if (!ExpectHex(crc.calc(checkInput, (int)std::size(checkInput), CRC_INITIAL), 0x0376E6E7u,
        _T("CRC-32/MPEG-2の検査値"), diagnostic)) return false;
    if (!ExpectHex(crc.calc(checkInput, 0, CRC_INITIAL), CRC_INITIAL, _T("長さ0のCRC"), diagnostic)) return false;
    // 分割して計算しても一括計算と一致する
    const uint32_t head = crc.calc(checkInput, 4, CRC_INITIAL);
    if (!ExpectHex(crc.calc(checkInput + 4, 5, head), 0x0376E6E7u, _T("分割計算のCRC"), diagnostic)) return false;

    // PSIセクション末尾にCRCをビッグエンディアンで付与すると、全体のCRCは0になる
    std::vector<uint8_t> section = { 0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xE1, 0x00 };
    const uint32_t sectionCRC = crc.calc(section.data(), (int)section.size(), CRC_INITIAL);
    section.resize(section.size() + 4);
    write32(section.data() + section.size() - 4, sectionCRC);
    if (!ExpectHex(crc.calc(section.data(), (int)section.size(), CRC_INITIAL), 0, _T("CRC付きセクションの検査"), diagnostic)) return false;
    section[3] ^= 0x01;
    return Expect(crc.calc(section.data(), (int)section.size(), CRC_INITIAL) != 0,
        _T("1ビット破損したセクションのCRCが0になりました"), diagnostic);
}

// 符号なし指数ゴロム符号を書き込む (codeNum+1 のビット長-1 個の0を前置する)
void WriteExpGolomb(BitWriter& writer, uint32_t codeNum) {
    const uint32_t value = codeNum + 1;
    int bitLength = 0;
    for (uint32_t v = value; v != 0; v >>= 1) bitLength++;
    if (bitLength > 1) writer.writen(0, bitLength - 1);
    writer.writen(value, bitLength);
}

template <typename Func>
bool ExpectEOF(Func func) {
    try {
        func();
    } catch (const EOFException&) {
        return true;
    }
    return false;
}

bool TestBitReader(tstring& diagnostic) {
    const uint8_t data[] = { 0xA5, 0x3C, 0xFF, 0x00, 0x81 };
    {
        BitReader reader(MemoryChunk(const_cast<uint8_t*>(data), std::size(data)));
        if (!ExpectHex(reader.read<4>(), 0xA, _T("上位4ビット"), diagnostic)) return false;
        if (!ExpectHex(reader.next<4>(), 0x5, _T("先読み4ビット"), diagnostic)) return false;
        if (!ExpectHex(reader.read<4>(), 0x5, _T("先読み後の4ビット"), diagnostic)) return false;
        if (!ExpectHex(reader.readn(12), 0x3CF, _T("バイト境界をまたぐ12ビット"), diagnostic)) return false;
        if (!Expect(reader.numReadBytes() == 3, _T("途中まで読んだバイトを1バイトとして数えません"), diagnostic)) return false;
        reader.skip(1);
        if (!ExpectHex(reader.readn(3), 0x7, _T("skip後の3ビット"), diagnostic)) return false;
        reader.byteAlign();
        if (!ExpectHex(reader.read<8>(), 0x00, _T("byteAlign後のバイト"), diagnostic)) return false;
        if (!Expect(reader.canRead(8) && !reader.canRead(9), _T("残りビット数の判定が一致しません"), diagnostic)) return false;
        if (!ExpectHex(reader.read<8>(), 0x81, _T("末尾バイト"), diagnostic)) return false;
        if (!Expect(ExpectEOF([&] { reader.read<1>(); }), _T("終端を越えて読んでもEOFExceptionになりません"), diagnostic)) return false;
    }
    {
        BitReader reader(MemoryChunk(const_cast<uint8_t*>(data), std::size(data)));
        if (!ExpectHex(reader.readn(32), 0xA53CFF00u, _T("32ビット読み出し"), diagnostic)) return false;
        if (!Expect(ExpectEOF([&] { reader.skip(9); }), _T("終端を越えるskipがEOFExceptionになりません"), diagnostic)) return false;
    }

    // 指数ゴロム符号: 規格の例 (1→0, 010→1, 011→2, 00100→3, 00111→6) と符号付き変換
    {
        AutoBuffer buffer;
        BitWriter writer(buffer);
        for (const uint32_t codeNum : { 0u, 1u, 2u, 3u, 6u, 1u, 2u, 3u, 4u }) WriteExpGolomb(writer, codeNum);
        writer.byteAlign<false>();
        writer.flush();
        const uint8_t expectedHead[] = { 0xA6, 0x43, 0xA6 }; // 1 010 011 00100 00111 010 011 0...
        if (!Expect(buffer.size() >= std::size(expectedHead) && std::memcmp(buffer.ptr(), expectedHead, std::size(expectedHead)) == 0,
            _T("指数ゴロム符号の書き込み結果が一致しません"), diagnostic)) return false;
        BitReader reader(buffer.get());
        for (const uint32_t expected : { 0u, 1u, 2u, 3u, 6u }) {
            if (!ExpectHex(reader.readExpGolom(), expected, _T("指数ゴロム符号"), diagnostic)) return false;
        }
        for (const int32_t expected : { 1, -1, 2, -2 }) {
            const int32_t actual = reader.readExpGolomSigned();
            if (actual != expected) {
                diagnostic = strsprintf(_T("符号付き指数ゴロム符号: 期待値=%d, 実際=%d"), expected, actual);
                return false;
            }
        }
    }

    // 64ビットの内部バッファ境界をまたぐ位置でも読み書きが往復する
    {
        constexpr int VALUE_COUNT = 300;
        AutoBuffer buffer;
        BitWriter writer(buffer);
        std::vector<uint32_t> values;
        uint32_t seed = 12345;
        for (int i = 0; i < VALUE_COUNT; i++) {
            seed = seed * 1103515245u + 12345u;
            values.push_back((seed >> 8) & ((1u << (i % 16)) - 1));
            writer.writen(i & 1, 1 + (i % 7)); // ずれを作るための固定長フィールド
            WriteExpGolomb(writer, values.back());
        }
        writer.byteAlign<true>();
        writer.flush();
        BitReader reader(buffer.get());
        for (int i = 0; i < VALUE_COUNT; i++) {
            if (!ExpectHex(reader.readn(1 + (i % 7)), i & 1, _T("固定長フィールドの往復"), diagnostic)) return false;
            if (!ExpectHex(reader.readExpGolom(), values[i], _T("指数ゴロム符号の往復"), diagnostic)) return false;
        }
        while (reader.canRead(1)) {
            if (!ExpectHex(reader.read<1>(), 1, _T("byteAlign<true>のパディング"), diagnostic)) return false;
        }
    }

    // バイト境界にない状態でのflushは例外になる
    AutoBuffer unaligned;
    BitWriter unalignedWriter(unaligned);
    unalignedWriter.write<3>(0x5);
    try {
        unalignedWriter.flush();
    } catch (const FormatException&) {
        return true;
    }
    diagnostic = _T("バイト境界にないflushがFormatExceptionになりません");
    return false;
}

bool TestAutoBuffer(tstring& diagnostic) {
    AutoBuffer buffer;
    if (!Expect(buffer.size() == 0, _T("初期サイズが0ではありません"), diagnostic)) return false;
    uint8_t chunk[] = { 1, 2, 3, 4, 5 };
    buffer.add(MemoryChunk(chunk, std::size(chunk)));
    buffer.add(uint8_t(6));
    if (!Expect(buffer.size() == 6 && buffer.ptr()[5] == 6, _T("追加したデータが一致しません"), diagnostic)) return false;
    buffer.trimHead(2);
    if (!Expect(buffer.size() == 4 && buffer.ptr()[0] == 3, _T("trimHead後の先頭が一致しません"), diagnostic)) return false;
    buffer.trimTail(1);
    if (!Expect(buffer.size() == 3 && buffer.get().data[2] == 5, _T("trimTail後の末尾が一致しません"), diagnostic)) return false;
    buffer.trimTail(10);
    if (!Expect(buffer.size() == 0, _T("サイズを超えるtrimTailで空になりません"), diagnostic)) return false;

    // space/extendで直接書き込んだ領域がデータとして扱われる
    MemoryChunk space = buffer.space(3);
    if (!Expect(space.length >= 3, _T("space()が要求サイズを確保しません"), diagnostic)) return false;
    space.data[0] = 7; space.data[1] = 8; space.data[2] = 9;
    buffer.extend(3);
    if (!Expect(buffer.size() == 3 && buffer.ptr()[0] == 7 && buffer.ptr()[2] == 9, _T("extend後のデータが一致しません"), diagnostic)) return false;
    buffer.trimHead(10);
    if (!Expect(buffer.size() == 0, _T("サイズを超えるtrimHeadで空になりません"), diagnostic)) return false;

    // 先頭を削りながら追加を繰り返しても、再配置後のデータ順序が保たれる
    constexpr int TOTAL_BYTES = 100000;
    constexpr int KEEP_BYTES = 777;
    int nextValue = 0;
    int headValue = 0;
    for (int i = 0; i < TOTAL_BYTES; i++) {
        buffer.add(uint8_t(nextValue++ & 0xFF));
        if ((int)buffer.size() > KEEP_BYTES) {
            const int trim = (int)buffer.size() - KEEP_BYTES + (i % 5);
            buffer.trimHead(trim);
            headValue += std::min(trim, nextValue - headValue);
        }
    }
    if (!Expect((int)buffer.size() == nextValue - headValue, _T("繰り返し追加後のサイズが一致しません"), diagnostic)) return false;
    for (size_t i = 0; i < buffer.size(); i++) {
        if (buffer.ptr()[i] != uint8_t((headValue + i) & 0xFF)) {
            diagnostic = strsprintf(_T("再配置後のデータ順序が崩れています: 位置=%zu"), i);
            return false;
        }
    }
    buffer.clear();
    if (!Expect(buffer.size() == 0, _T("clear後に空になりません"), diagnostic)) return false;
    buffer.release();
    buffer.add(uint8_t(42));
    return Expect(buffer.size() == 1 && buffer.ptr()[0] == 42, _T("release後に再利用できません"), diagnostic);
}

bool TestEncoderOption(tstring& diagnostic) {
    struct DeintCase {
        int encoder;
        const tchar* options;
        int expectedDeint;
        int expectedTimecode;
    };
    // 旧EncoderOptionTest01-09と、presetと個別指定の適用順のケース
    const DeintCase deintCases[] = {
        { ENCODER_QSVENC, _T("--vpp-deinterlace none"), ENCODER_DEINT_NONE, 0 },
        { ENCODER_QSVENC, _T("--vpp-deinterlace normal"), ENCODER_DEINT_30P, 0 },
        { ENCODER_QSVENC, _T("--vpp-deinterlace adaptive"), ENCODER_DEINT_30P, 0 },
        { ENCODER_QSVENC, _T("--vpp-deinterlace it"), ENCODER_DEINT_24P, 0 },
        { ENCODER_QSVENC, _T("--vpp-deinterlace bob"), ENCODER_DEINT_60P, 0 },
        { ENCODER_QSVENC, _T("--vpp-afs preset=anime,24fps=true,rff=true"), ENCODER_DEINT_24P, 0 },
        { ENCODER_QSVENC, _T("--vpp-afs preset=anime"), ENCODER_DEINT_30P, 0 },
        { ENCODER_QSVENC, _T("--vpp-afs preset=24fps"), ENCODER_DEINT_24P, 0 },
        // エンコーダはpresetを記述順に関係なく先に適用するため、後置のpresetで24fps指定は消えない
        { ENCODER_QSVENC, _T("--vpp-afs 24fps=true,preset=anime"), ENCODER_DEINT_24P, 0 },
        { ENCODER_QSVENC, _T("--vpp-afs 24fps=false,preset=24fps"), ENCODER_DEINT_30P, 0 },
        { ENCODER_NVENC, _T("--vpp-afs preset=anime,timecode=true"), ENCODER_DEINT_VFR, 1 },
        { ENCODER_QSVENC, _T("-i %1 --avqsv --cqp 22:24:26 -u best --output-res 1280x720 --vpp-denoise 20 --tff --vpp-deinterlace normal --trellis auto --bframes 2 --gop-len 300 --audio-codec aac --audio-bitrate 128 -o \"dpn1.mp4\" --vpp-afs preset=anime,rff=true,24fps=true"),
            ENCODER_DEINT_24P, 0 },
        // x264にはvppオプションがないため無視される
        { ENCODER_X264, _T("--vpp-deinterlace bob"), ENCODER_DEINT_NONE, 0 },
    };
    for (const auto& testCase : deintCases) {
        EncoderOptionInfoForTest info = {};
        const int status = ParseEncoderOptionForTest(testCase.encoder, testCase.options, &info);
        if (status != ENCODER_OPTION_FOR_TEST_SUCCESS || info.deint != testCase.expectedDeint
            || info.afsTimecode != testCase.expectedTimecode) {
            diagnostic = strsprintf(_T("インタレ解除判定: 結果=%d, deint=%d/%d, timecode=%d/%d, オプション=%s"),
                status, info.deint, testCase.expectedDeint, info.afsTimecode, testCase.expectedTimecode, testCase.options);
            return false;
        }
    }

    // 24fps化に間引きが伴わない指定はエラー (afsのdropの既定値はoff)
    EncoderOptionInfoForTest info = {};
    if (!Expect(ParseEncoderOptionForTest(ENCODER_QSVENC, _T("--vpp-afs 24fps=true,drop=false"), &info)
        == ENCODER_OPTION_FOR_TEST_PARSE_FAILED, _T("drop=falseの24fps化を拒否できません"), diagnostic)) return false;
    if (!Expect(ParseEncoderOptionForTest(ENCODER_QSVENC, _T("--vpp-deinterlace normal --vpp-afs rff=true,24fps=true"), &info)
        == ENCODER_OPTION_FOR_TEST_PARSE_FAILED, _T("drop未指定の24fps化を拒否できません"), diagnostic)) return false;
    if (!Expect(ParseEncoderOptionForTest(ENCODER_QSVENC, nullptr, &info) == ENCODER_OPTION_FOR_TEST_INVALID_ARGUMENT,
        _T("nullptrのオプションを拒否できません"), diagnostic)) return false;

    struct RCCase {
        int encoder;
        const tchar* options;
        int expectedFormat;
        const char* expectedRCMode;
        double expectedValues[3];
    };
    const RCCase rcCases[] = {
        { ENCODER_X264, _T(""), VS_H264, "crf", { 23, 0, 0 } },
        { ENCODER_X265, _T("--crf 20.5"), VS_H265, "crf", { 20.5, 0, 0 } },
        { ENCODER_X264, _T("--bitrate 5000"), VS_H264, "bitrate", { 5000, 0, 0 } },
        { ENCODER_X262, _T(""), VS_MPEG2, "crf", { 23, 0, 0 } },
        { ENCODER_SVTAV1, _T("--rc 1 --tbr 3000"), VS_AV1, "tbr", { 3000, 0, 0 } },
        { ENCODER_SVTAV1, _T("--rc 0"), VS_AV1, "crf", { 35, 0, 0 } },
        { ENCODER_QSVENC, _T(""), VS_H264, "icq", { 23, 0, 0 } },
        { ENCODER_QSVENC, _T("-c hevc --cqp 22:24:26"), VS_H265, "cqp", { 22, 24, 26 } },
        { ENCODER_QSVENC, _T("--cqp 20:23"), VS_H264, "cqp", { 20, 23, 23 } },
        { ENCODER_QSVENC, _T("--cqp 18"), VS_H264, "cqp", { 18, 18, 18 } },
        { ENCODER_NVENC, _T("--codec av1 --qvbr 30.5"), VS_AV1, "qvbr", { 30.5, 0, 0 } },
        { ENCODER_NVENC, _T("--vbr 0 --vbr-quality 28"), VS_H264, "qvbr", { 28, 0, 0 } },
        { ENCODER_NVENC, _T("--codec mpeg2 --cbr 8000"), VS_MPEG2, "cbr", { 8000, 0, 0 } },
    };
    for (const auto& testCase : rcCases) {
        info = {};
        const int status = ParseEncoderOptionForTest(testCase.encoder, testCase.options, &info);
        bool matched = status == ENCODER_OPTION_FOR_TEST_SUCCESS && info.format == testCase.expectedFormat
            && std::strcmp(info.rcMode, testCase.expectedRCMode) == 0;
        for (int i = 0; matched && i < 3; i++) matched = std::abs(info.rcModeValue[i] - testCase.expectedValues[i]) < 1e-9;
        if (!matched) {
            diagnostic = strsprintf(_T("レート制御解析: 結果=%d, format=%d/%d, mode=%s/%s, 値=%.2f:%.2f:%.2f, オプション=%s"),
                status, info.format, testCase.expectedFormat, char_to_tstring(info.rcMode).c_str(),
                char_to_tstring(testCase.expectedRCMode).c_str(), info.rcModeValue[0], info.rcModeValue[1], info.rcModeValue[2],
                testCase.options);
            return false;
        }
    }

    struct ParallelCase {
        int encoder;
        const tchar* options;
        int expectedParallel;
        int expectedSelectEvery;
    };
    const ParallelCase parallelCases[] = {
        { ENCODER_NVENC, _T(""), 0, 1 },
        { ENCODER_NVENC, _T("--parallel 2"), 2, 1 },
        // autoはAmatsukaze側で並列数を決められないため2並列として扱う
        { ENCODER_NVENC, _T("--parallel auto"), 2, 1 },
        { ENCODER_QSVENC, _T("--parallel mp=3,chunks=8"), 3, 1 },
        { ENCODER_QSVENC, _T("--parallel --cqp 20"), 0, 1 },
        { ENCODER_NVENC, _T("--vpp-select-every 2"), 0, 2 },
        { ENCODER_NVENC, _T("--vpp-select-every step=5,offset=1"), 0, 5 },
    };
    for (const auto& testCase : parallelCases) {
        info = {};
        const int status = ParseEncoderOptionForTest(testCase.encoder, testCase.options, &info);
        if (status != ENCODER_OPTION_FOR_TEST_SUCCESS || info.parallel != testCase.expectedParallel
            || info.selectEvery != testCase.expectedSelectEvery) {
            diagnostic = strsprintf(_T("並列/間引き解析: 結果=%d, parallel=%d/%d, selectEvery=%d/%d, オプション=%s"),
                status, info.parallel, testCase.expectedParallel, info.selectEvery, testCase.expectedSelectEvery,
                testCase.options);
            return false;
        }
    }
    return true;
}

// テスト用ビットストリームを組み立てる (書き込んだビット数を保持し、SEIペイロード等の整列に使う)
class BitstreamBuilder {
public:
    BitstreamBuilder() : writer(buffer), bitCount(0) {}
    BitstreamBuilder& bits(uint32_t value, int count) {
        if (count > 0) writer.writen(value, count);
        bitCount += count;
        return *this;
    }
    BitstreamBuilder& flag(bool value) { return bits(value ? 1 : 0, 1); }
    // 符号なし指数ゴロム符号 ue(v)
    BitstreamBuilder& ue(uint32_t codeNum) {
        const uint32_t value = codeNum + 1;
        int bitLength = 0;
        for (uint32_t v = value; v != 0; v >>= 1) bitLength++;
        bits(0, bitLength - 1);
        return bits(value, bitLength);
    }
    // 符号付き指数ゴロム符号 se(v)
    BitstreamBuilder& se(int32_t value) {
        return ue(value > 0 ? (uint32_t)(2 * value - 1) : (uint32_t)(-2 * value));
    }
    // バイト境界まで0で埋める
    BitstreamBuilder& alignZero() {
        while (bitCount % 8 != 0) bits(0, 1);
        return *this;
    }
    // rbsp_trailing_bits / SEIペイロードの整列 (1を書いてから0で埋める)
    BitstreamBuilder& trailingBits() {
        bits(1, 1);
        return alignZero();
    }
    BitstreamBuilder& bytes(const std::vector<uint8_t>& data) {
        for (const auto b : data) bits(b, 8);
        return *this;
    }
    bool isAligned() const { return bitCount % 8 == 0; }
    std::vector<uint8_t> finish() {
        alignZero();
        writer.flush();
        return std::vector<uint8_t>(buffer.ptr(), buffer.ptr() + buffer.size());
    }
private:
    AutoBuffer buffer;
    BitWriter writer;
    size_t bitCount;
};

// 映像パーサにアクセスユニット列を入力し、フレーム情報と各ユニットの戻り値を得る
bool ParseVideoUnits(int streamFormat, const std::vector<std::vector<uint8_t>>& units,
    const std::vector<std::pair<int64_t, int64_t>>& timestamps, std::vector<VideoFrameInfo>& frames,
    std::vector<int>& unitResults, tstring& diagnostic) {
    std::vector<VideoAccessUnitForTest> inputs;
    for (size_t i = 0; i < units.size(); i++) {
        inputs.push_back({ units[i].data(), units[i].size(), timestamps[i].first, timestamps[i].second });
    }
    unitResults.assign(units.size(), -1);
    size_t frameCount = 0;
    int status = ParseVideoAccessUnitsForTest(streamFormat, inputs.data(), inputs.size(), unitResults.data(),
        nullptr, 0, &frameCount);
    if (status != VIDEO_PARSER_FOR_TEST_SUCCESS) {
        diagnostic = strsprintf(_T("映像パーサを実行できません: 結果=%d"), status);
        return false;
    }
    frames.assign(frameCount, VideoFrameInfo());
    status = ParseVideoAccessUnitsForTest(streamFormat, inputs.data(), inputs.size(), unitResults.data(),
        frames.data(), frames.size(), &frameCount);
    if (status != VIDEO_PARSER_FOR_TEST_SUCCESS || frameCount != frames.size()) {
        diagnostic = strsprintf(_T("映像パーサのフレーム情報を取得できません: 結果=%d"), status);
        return false;
    }
    return true;
}

struct ExpectedVideoFrame {
    PICTURE_TYPE pic;
    FRAME_TYPE type;
    bool isGopStart;
    int64_t PTS;
    int64_t DTS;
};

bool ExpectVideoFrame(const VideoFrameInfo& actual, const ExpectedVideoFrame& expected, size_t index, tstring& diagnostic) {
    if (actual.pic == expected.pic && actual.type == expected.type && actual.isGopStart == expected.isGopStart
        && actual.PTS == expected.PTS && actual.DTS == expected.DTS) {
        return true;
    }
    diagnostic = strsprintf(_T("フレーム%zu: pic=%d/%d, type=%d/%d, GOP先頭=%d/%d, PTS=%lld/%lld, DTS=%lld/%lld"), index,
        actual.pic, expected.pic, actual.type, expected.type, actual.isGopStart, expected.isGopStart,
        (long long)actual.PTS, (long long)expected.PTS, (long long)actual.DTS, (long long)expected.DTS);
    return false;
}

struct ExpectedVideoFormat {
    int width, height, displayWidth, displayHeight;
    int sarWidth, sarHeight;
    int frameRateNum, frameRateDenom;
    int colorPrimaries;
    bool progressive;
};

bool ExpectVideoFormat(const VideoFormat& actual, const ExpectedVideoFormat& expected, tstring& diagnostic) {
    if (actual.width == expected.width && actual.height == expected.height
        && actual.displayWidth == expected.displayWidth && actual.displayHeight == expected.displayHeight
        && actual.sarWidth == expected.sarWidth && actual.sarHeight == expected.sarHeight
        && actual.frameRateNum == expected.frameRateNum && actual.frameRateDenom == expected.frameRateDenom
        && actual.colorPrimaries == expected.colorPrimaries && actual.progressive == expected.progressive) {
        return true;
    }
    diagnostic = strsprintf(_T("映像フォーマット: %dx%d/%dx%d, 表示%dx%d/%dx%d, SAR=%d:%d/%d:%d, fps=%d/%d / %d/%d, 色=%d/%d, progressive=%d/%d"),
        actual.width, actual.height, expected.width, expected.height,
        actual.displayWidth, actual.displayHeight, expected.displayWidth, expected.displayHeight,
        actual.sarWidth, actual.sarHeight, expected.sarWidth, expected.sarHeight,
        actual.frameRateNum, actual.frameRateDenom, expected.frameRateNum, expected.frameRateDenom,
        actual.colorPrimaries, expected.colorPrimaries, actual.progressive, expected.progressive);
    return false;
}

// MPEG-2 映像ビットストリームの組み立て
constexpr uint32_t MPEG2_SEQUENCE_HEADER_CODE = 0x000001B3;
constexpr uint32_t MPEG2_EXTENSION_START_CODE = 0x000001B5;
constexpr uint32_t MPEG2_PICTURE_START_CODE = 0x00000100;
constexpr uint32_t MPEG2_GOP_START_CODE = 0x000001B8;
constexpr uint32_t MPEG2_FIRST_SLICE_START_CODE = 0x00000101;
constexpr int MPEG2_TOP_FIELD = 1;
constexpr int MPEG2_BOTTOM_FIELD = 2;
constexpr int MPEG2_FRAME_PICTURE = 3;
constexpr int MPEG2_CODING_I = 1;
constexpr int MPEG2_CODING_P = 2;
constexpr int MPEG2_CODING_B = 3;

struct Mpeg2SequenceParams {
    int width, height;
    int aspectRatioInfo;
    int frameRateCode;
    bool progressiveSequence;
    uint32_t bitRateValue;   // 400bps単位、下位18bitがヘッダ、上位12bitが拡張
    uint32_t vbvBufferSize;  // 下位10bitがヘッダ、上位8bitが拡張
    bool hasDisplayExtension;
    int displayWidth, displayHeight;
    bool hasColourDescription;
    uint8_t colour;          // colour_primaries/transfer/matrixに同じ値を入れる
};

void AppendMpeg2Sequence(BitstreamBuilder& b, const Mpeg2SequenceParams& p) {
    constexpr uint32_t PROFILE_AND_LEVEL_MP_HL = 0x44;
    constexpr uint32_t CHROMA_420 = 1;
    b.bits(MPEG2_SEQUENCE_HEADER_CODE, 32).bits(p.width & 0xFFF, 12).bits(p.height & 0xFFF, 12)
        .bits(p.aspectRatioInfo, 4).bits(p.frameRateCode, 4).bits(p.bitRateValue & 0x3FFFF, 18).bits(1, 1)
        .bits(p.vbvBufferSize & 0x3FF, 10).bits(0, 1).bits(0, 1).bits(0, 1).alignZero();
    b.bits(MPEG2_EXTENSION_START_CODE, 32).bits(1, 4).bits(PROFILE_AND_LEVEL_MP_HL, 8).flag(p.progressiveSequence)
        .bits(CHROMA_420, 2).bits(p.width >> 12, 2).bits(p.height >> 12, 2).bits(p.bitRateValue >> 18, 12).bits(1, 1)
        .bits(p.vbvBufferSize >> 10, 8).bits(0, 1).bits(0, 2).bits(0, 5).alignZero();
    if (p.hasDisplayExtension) {
        constexpr uint32_t VIDEO_FORMAT_UNSPECIFIED = 5;
        b.bits(MPEG2_EXTENSION_START_CODE, 32).bits(2, 4).bits(VIDEO_FORMAT_UNSPECIFIED, 3).flag(p.hasColourDescription);
        if (p.hasColourDescription) b.bits(p.colour, 8).bits(p.colour, 8).bits(p.colour, 8);
        b.bits(p.displayWidth, 14).bits(1, 1).bits(p.displayHeight, 14).alignZero();
    }
}

struct Mpeg2PictureParams {
    int codingType;
    int structure;
    bool topFieldFirst;
    bool repeatFirstField;
    bool progressiveFrame;
};

void AppendMpeg2Picture(BitstreamBuilder& b, const Mpeg2PictureParams& p) {
    constexpr uint32_t VBV_DELAY_VARIABLE = 0xFFFF;
    constexpr uint32_t F_CODE_UNUSED = 7;
    b.bits(MPEG2_PICTURE_START_CODE, 32).bits(0, 10).bits(p.codingType, 3).bits(VBV_DELAY_VARIABLE, 16);
    if (p.codingType == MPEG2_CODING_P || p.codingType == MPEG2_CODING_B) b.bits(0, 1).bits(F_CODE_UNUSED, 3);
    if (p.codingType == MPEG2_CODING_B) b.bits(0, 1).bits(F_CODE_UNUSED, 3);
    b.bits(0, 1).alignZero(); // extra_bit_picture
    // picture coding extension
    b.bits(MPEG2_EXTENSION_START_CODE, 32).bits(8, 4).bits(0xFFFF, 16).bits(0, 2).bits(p.structure, 2)
        .flag(p.topFieldFirst).flag(p.structure == MPEG2_FRAME_PICTURE).bits(0, 1).bits(0, 1).bits(0, 1).bits(0, 1)
        .flag(p.repeatFirstField).flag(p.progressiveFrame).flag(p.progressiveFrame).bits(0, 1).alignZero();
    // スライス (中身はパーサで解析されない)
    b.bits(MPEG2_FIRST_SLICE_START_CODE, 32).bits(0x12345678, 32);
}

std::vector<uint8_t> MakeMpeg2Unit(const Mpeg2SequenceParams* sequence, std::initializer_list<Mpeg2PictureParams> pictures,
    bool withGopHeader = false) {
    BitstreamBuilder b;
    if (sequence) AppendMpeg2Sequence(b, *sequence);
    if (withGopHeader) {
        // time_code(drop_frame, 時, 分, marker, 秒, ピクチャ) = 0:00:00.00, closed_gop=1
        b.bits(MPEG2_GOP_START_CODE, 32).bits(0, 1).bits(0, 5).bits(0, 6).bits(1, 1).bits(0, 6).bits(0, 6)
            .bits(1, 1).bits(0, 1).alignZero();
    }
    for (const auto& picture : pictures) AppendMpeg2Picture(b, picture);
    return b.finish();
}

bool TestMpeg2VideoParser(tstring& diagnostic) {
    // 1440x1080 16:9 29.97fps インタレース (BSデジタル放送の典型)
    // 先頭はGOPヘッダを省略し、シーケンス拡張の直後にピクチャヘッダが続く (規格上許される配置)
    const Mpeg2SequenceParams interlaced = { 1440, 1080, 3, 4, false, 50000, 488, false, 0, 0, false, 0 };
    const std::vector<std::vector<uint8_t>> units = {
        MakeMpeg2Unit(&interlaced, { { MPEG2_CODING_I, MPEG2_FRAME_PICTURE, true, false, false } }),
        MakeMpeg2Unit(nullptr, { { MPEG2_CODING_P, MPEG2_FRAME_PICTURE, false, false, false } }),
        MakeMpeg2Unit(nullptr, { { MPEG2_CODING_B, MPEG2_FRAME_PICTURE, true, true, false } }),
        MakeMpeg2Unit(nullptr, { { MPEG2_CODING_B, MPEG2_FRAME_PICTURE, false, true, false } }),
        // フィールドピクチャのペアは1フレームになる
        MakeMpeg2Unit(nullptr, { { MPEG2_CODING_I, MPEG2_TOP_FIELD, false, false, false },
            { MPEG2_CODING_P, MPEG2_BOTTOM_FIELD, false, false, false } }),
        MakeMpeg2Unit(nullptr, { { MPEG2_CODING_P, MPEG2_BOTTOM_FIELD, false, false, false },
            { MPEG2_CODING_P, MPEG2_TOP_FIELD, false, false, false } }),
        // 同じパリティのフィールドが続く不正な配置は拒否される
        MakeMpeg2Unit(nullptr, { { MPEG2_CODING_P, MPEG2_TOP_FIELD, false, false, false },
            { MPEG2_CODING_P, MPEG2_TOP_FIELD, false, false, false } }),
    };
    std::vector<std::pair<int64_t, int64_t>> timestamps;
    for (size_t i = 0; i < units.size(); i++) timestamps.emplace_back(1000 + 3003 * (int64_t)i, 900 + 3003 * (int64_t)i);
    std::vector<VideoFrameInfo> frames;
    std::vector<int> unitResults;
    if (!ParseVideoUnits(VS_MPEG2, units, timestamps, frames, unitResults, diagnostic)) return false;
    const std::vector<int> expectedResults = { 1, 1, 1, 1, 1, 1, 0 };
    if (unitResults != expectedResults) {
        diagnostic = _T("MPEG-2のアクセスユニットごとの解析結果が一致しません:");
        for (const auto r : unitResults) diagnostic += strsprintf(_T(" %d"), r);
        return false;
    }
    const ExpectedVideoFrame expectedFrames[] = {
        { PIC_TFF, FRAME_I, true, 1000, 900 },
        { PIC_BFF, FRAME_P, false, 4003, 3903 },
        { PIC_TFF_RFF, FRAME_B, false, 7006, 6906 },
        { PIC_BFF_RFF, FRAME_B, false, 10009, 9909 },
        { PIC_TFF, FRAME_I, false, 13012, 12912 },
        { PIC_BFF, FRAME_P, false, 16015, 15915 },
    };
    if (!Expect(frames.size() == std::size(expectedFrames), _T("MPEG-2のフレーム数が一致しません"), diagnostic)) return false;
    for (size_t i = 0; i < frames.size(); i++) {
        if (!ExpectVideoFrame(frames[i], expectedFrames[i], i, diagnostic)) return false;
    }
    // SAR = DAR(16:9) * 1080 / 1440 = 4:3、色情報なしは2(未指定)
    if (!ExpectVideoFormat(frames[0].format, { 1440, 1080, 1440, 1080, 4, 3, 30000, 1001, 2, false }, diagnostic)) return false;
    if (!Expect(frames[0].format.format == VS_MPEG2 && frames[0].format.mpeg2BitRateValue == 50000
        && frames[0].format.mpeg2VbvBufferSizeValue == 488 && frames[0].format.mpeg2ProfileAndLevelIndication == 0x44,
        _T("MPEG-2のビットレート/VBV/プロファイルが一致しません"), diagnostic)) return false;
    if (!Expect(!frames[0].progressive, _T("インタレースのフレームがprogressive扱いです"), diagnostic)) return false;

    // 長い非ヘッダー領域、偽候補、末尾の不完全コードを挟んでも解析結果を維持する。
    auto paddedUnits = units;
    for (auto& unit : paddedUnits) {
        std::vector<uint8_t> prefix(4096, 0x55);
        prefix.insert(prefix.end(), { 1, 0, 1, 0xb3, 0, 1, 0, 0, 0 });
        unit.insert(unit.begin(), prefix.begin(), prefix.end());
        unit.insert(unit.end(), { 0, 0, 1 });
    }
    if (!ParseVideoUnits(VS_MPEG2, paddedUnits, timestamps, frames, unitResults, diagnostic)) return false;
    if (!Expect(unitResults == expectedResults && frames.size() == std::size(expectedFrames),
        _T("MPEG-2の候補探索でフレーム数または解析結果が変わりました"), diagnostic)) return false;
    for (size_t i = 0; i < frames.size(); i++) {
        if (!ExpectVideoFrame(frames[i], expectedFrames[i], i, diagnostic)) return false;
    }

    // 720x480 4:3 23.976fps プログレッシブ、表示領域と色情報あり、GOPヘッダあり
    const Mpeg2SequenceParams progressive = { 720, 480, 2, 1, true, 15000, 112, true, 704, 480, true, 1 };
    const std::vector<std::vector<uint8_t>> progressiveUnits = {
        MakeMpeg2Unit(&progressive, { { MPEG2_CODING_I, MPEG2_FRAME_PICTURE, false, false, true } }, true),
        MakeMpeg2Unit(nullptr, { { MPEG2_CODING_P, MPEG2_FRAME_PICTURE, false, true, true } }),
        MakeMpeg2Unit(nullptr, { { MPEG2_CODING_B, MPEG2_FRAME_PICTURE, true, true, true } }),
    };
    const std::vector<std::pair<int64_t, int64_t>> noTimestamps(progressiveUnits.size(), { -1, -1 });
    if (!ParseVideoUnits(VS_MPEG2, progressiveUnits, noTimestamps, frames, unitResults, diagnostic)) return false;
    const ExpectedVideoFrame progressiveFrames[] = {
        { PIC_FRAME, FRAME_I, true, -1, -1 },
        { PIC_FRAME_DOUBLING, FRAME_P, false, -1, -1 },
        { PIC_FRAME_TRIPLING, FRAME_B, false, -1, -1 },
    };
    if (!Expect(frames.size() == std::size(progressiveFrames), _T("プログレッシブMPEG-2のフレーム数が一致しません"), diagnostic)) return false;
    for (size_t i = 0; i < frames.size(); i++) {
        if (!ExpectVideoFrame(frames[i], progressiveFrames[i], i, diagnostic)) return false;
    }
    // SAR = DAR(4:3) * 480 / 704 = 10:11
    if (!ExpectVideoFormat(frames[0].format, { 720, 480, 704, 480, 10, 11, 24000, 1001, 1, true }, diagnostic)) return false;
    return Expect(frames[0].progressive, _T("プログレッシブのフレームがprogressive扱いになりません"), diagnostic);
}

// H.264 映像ビットストリームの組み立て
constexpr uint8_t H264_NAL_SLICE = 0x21;      // nal_ref_idc=1, type=1
constexpr uint8_t H264_NAL_IDR_SLICE = 0x65;  // nal_ref_idc=3, type=5
constexpr uint8_t H264_NAL_SEI = 0x06;
constexpr uint8_t H264_NAL_SPS = 0x67;
constexpr uint8_t H264_NAL_PPS = 0x68;
constexpr uint8_t H264_NAL_AUD = 0x09;
constexpr int H264_AUD_I = 0;
constexpr int H264_AUD_P = 1;
constexpr int H264_AUD_B = 2;
constexpr uint32_t H264_HRD_DELAY_LENGTH = 24;

// スタートコードとエミュレーション防止バイトを付けてNALユニットを追加する
void AppendNal(std::vector<uint8_t>& out, uint8_t header, const std::vector<uint8_t>& rbsp) {
    out.insert(out.end(), { 0x00, 0x00, 0x00, 0x01, header });
    int zeroCount = 0;
    for (const auto b : rbsp) {
        if (zeroCount >= 2 && b <= 0x03) {
            out.push_back(0x03);
            zeroCount = 0;
        }
        out.push_back(b);
        zeroCount = (b == 0) ? zeroCount + 1 : 0;
    }
}

// 1440x1080 インタレース、4:3 SAR、29.97fps、NAL HRDとpic_structありのSPS (BSデジタル放送のH.264を想定)
std::vector<uint8_t> MakeH264Sps() {
    constexpr uint32_t PROFILE_HIGH = 100;
    constexpr uint32_t LEVEL_40 = 40;
    constexpr uint32_t ASPECT_RATIO_IDC_4_3 = 14;
    BitstreamBuilder b;
    b.bits(PROFILE_HIGH, 8).bits(0, 8).bits(LEVEL_40, 8).ue(0) // profile, constraint flags, level, sps_id
        .ue(1).ue(0).ue(0).bits(0, 1).bits(0, 1)                // 4:2:0, 8bit, scaling matrixなし
        .ue(0).ue(0).ue(0)                                       // log2_max_frame_num, poc_type=0, log2_max_poc_lsb
        .ue(4).bits(0, 1)                                        // max_num_ref_frames, gaps
        .ue(1440 / 16 - 1).ue(1088 / 32 - 1)                     // 幅90MB, 高さ34マップ単位(フィールド)
        .bits(0, 1).bits(1, 1).bits(1, 1)                        // frame_mbs_only=0, mbaff=1, direct_8x8
        .bits(1, 1).ue(0).ue(0).ue(0).ue(2)                      // 下を2単位(4:2:0インタレースで8ライン)クロップ
        .bits(1, 1)                                              // VUIあり
        .bits(1, 1).bits(ASPECT_RATIO_IDC_4_3, 8)
        .bits(0, 1)                                              // overscan
        .bits(1, 1).bits(5, 3).bits(0, 1).bits(1, 1).bits(1, 8).bits(1, 8).bits(1, 8) // BT.709
        .bits(0, 1)                                              // chroma_loc
        .bits(1, 1).bits(1001, 32).bits(60000, 32).bits(1, 1)    // timing (フィールドレート59.94)
        .bits(1, 1).ue(0).bits(0, 4).bits(0, 4).ue(1000).ue(1000).bits(0, 1) // NAL HRD
        .bits(H264_HRD_DELAY_LENGTH - 1, 5).bits(H264_HRD_DELAY_LENGTH - 1, 5).bits(H264_HRD_DELAY_LENGTH - 1, 5).bits(24, 5)
        .bits(0, 1).bits(0, 1)                                   // VCL HRDなし, low_delay
        .bits(1, 1).bits(0, 1)                                   // pic_struct_present, bitstream_restriction
        .trailingBits();
    return b.finish();
}

std::vector<uint8_t> MakeSeiMessage(int payloadType, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> message = { (uint8_t)payloadType, (uint8_t)payload.size() };
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<uint8_t> MakeBufferingPeriod() {
    BitstreamBuilder b;
    b.ue(0).bits(90000, H264_HRD_DELAY_LENGTH).bits(0, H264_HRD_DELAY_LENGTH);
    if (!b.isAligned()) b.trailingBits();
    return MakeSeiMessage(0, b.finish());
}

std::vector<uint8_t> MakePicTiming(int picStruct, uint32_t cpbRemovalDelay, uint32_t dpbOutputDelay) {
    // pic_structごとのNumClockTS
    constexpr int NUM_CLOCK_TS[] = { 1, 1, 1, 2, 2, 3, 3, 2, 3 };
    BitstreamBuilder b;
    b.bits(cpbRemovalDelay, H264_HRD_DELAY_LENGTH).bits(dpbOutputDelay, H264_HRD_DELAY_LENGTH).bits(picStruct, 4)
        .bits(0, NUM_CLOCK_TS[picStruct]); // clock_timestamp_flagはすべて0
    if (!b.isAligned()) b.trailingBits();
    return MakeSeiMessage(1, b.finish());
}

std::vector<uint8_t> MakePanScanRect(int left, int right, int top, int bottom) {
    BitstreamBuilder b;
    b.ue(0).bits(0, 1).ue(0).se(left).se(right).se(top).se(bottom).ue(0);
    if (!b.isAligned()) b.trailingBits();
    return MakeSeiMessage(2, b.finish());
}

void AppendSei(std::vector<uint8_t>& out, std::initializer_list<std::vector<uint8_t>> messages) {
    std::vector<uint8_t> rbsp;
    for (const auto& message : messages) rbsp.insert(rbsp.end(), message.begin(), message.end());
    rbsp.push_back(0x80); // rbsp_trailing_bits
    AppendNal(out, H264_NAL_SEI, rbsp);
}

void AppendAud(std::vector<uint8_t>& out, int primaryPicType) {
    AppendNal(out, H264_NAL_AUD, { (uint8_t)((primaryPicType << 5) | 0x10) });
}

void AppendSlice(std::vector<uint8_t>& out, uint8_t header) {
    AppendNal(out, header, { 0x88, 0x84, 0x21, 0xA0 }); // スライスの中身はパーサで解析されない
}

std::vector<uint8_t> MakeH264Unit(int primaryPicType, bool withParameterSets, std::initializer_list<std::vector<uint8_t>> seiMessages,
    int sliceCount = 1) {
    std::vector<uint8_t> unit;
    AppendAud(unit, primaryPicType);
    if (withParameterSets) {
        AppendNal(unit, H264_NAL_SPS, MakeH264Sps());
        AppendNal(unit, H264_NAL_PPS, BitstreamBuilder().ue(0).ue(0).trailingBits().finish());
    }
    AppendSei(unit, seiMessages);
    for (int i = 0; i < sliceCount; i++) AppendSlice(unit, withParameterSets ? H264_NAL_IDR_SLICE : H264_NAL_SLICE);
    return unit;
}

bool TestH264VideoParser(tstring& diagnostic) {
    constexpr int PIC_STRUCT_FRAME = 0;
    constexpr int PIC_STRUCT_TOP = 1;
    constexpr int PIC_STRUCT_BOTTOM = 2;
    constexpr int PIC_STRUCT_TB = 3;
    constexpr int PIC_STRUCT_BT = 4;
    constexpr int PIC_STRUCT_TBT = 5;
    constexpr int PIC_STRUCT_BTB = 6;
    constexpr int PIC_STRUCT_DOUBLING = 7;
    constexpr int PIC_STRUCT_TRIPLING = 8;
    // PESのPTS/DTSとSEIの遅延を一致させる: DTS = 先頭DTS + cpb_removal_delay * 1501.5、PTS = DTS + dpb_output_delay * 1501.5
    constexpr uint32_t DPB_DELAY = 2;
    std::vector<std::vector<uint8_t>> units = {
        MakeH264Unit(H264_AUD_I, true, { MakeBufferingPeriod(), MakePicTiming(PIC_STRUCT_TB, 0, DPB_DELAY) }),
        MakeH264Unit(H264_AUD_P, false, { MakePicTiming(PIC_STRUCT_BT, 2, DPB_DELAY) }),
        MakeH264Unit(H264_AUD_B, false, { MakePicTiming(PIC_STRUCT_TBT, 4, DPB_DELAY) }),
        MakeH264Unit(H264_AUD_B, false, { MakePicTiming(PIC_STRUCT_BTB, 6, DPB_DELAY) }),
        MakeH264Unit(H264_AUD_P, false, { MakePicTiming(PIC_STRUCT_FRAME, 8, DPB_DELAY) }),
        MakeH264Unit(H264_AUD_P, false, { MakePicTiming(PIC_STRUCT_DOUBLING, 10, DPB_DELAY) }),
        MakeH264Unit(H264_AUD_P, false, { MakePicTiming(PIC_STRUCT_TRIPLING, 12, DPB_DELAY) }),
    };
    // フィールドごとにSEIとスライスを持つアクセスユニットは1フレームになる
    std::vector<uint8_t> fieldPair;
    AppendAud(fieldPair, H264_AUD_P);
    AppendSei(fieldPair, { MakePicTiming(PIC_STRUCT_TOP, 14, DPB_DELAY) });
    AppendSlice(fieldPair, H264_NAL_SLICE);
    AppendSei(fieldPair, { MakePicTiming(PIC_STRUCT_BOTTOM, 15, DPB_DELAY) });
    AppendSlice(fieldPair, H264_NAL_SLICE);
    units.push_back(fieldPair);

    std::vector<std::pair<int64_t, int64_t>> timestamps;
    for (size_t i = 0; i < units.size(); i++) timestamps.emplace_back(10003 + 3003 * (int64_t)i, 7000 + 3003 * (int64_t)i);
    std::vector<VideoFrameInfo> frames;
    std::vector<int> unitResults;
    if (!ParseVideoUnits(VS_H264, units, timestamps, frames, unitResults, diagnostic)) return false;
    if (!Expect(std::all_of(unitResults.begin(), unitResults.end(), [](int r) { return r == 1; }),
        _T("H.264のアクセスユニットの解析に失敗しました"), diagnostic)) return false;
    const ExpectedVideoFrame expectedFrames[] = {
        { PIC_TFF, FRAME_I, true, 10003, 7000 },
        { PIC_BFF, FRAME_P, false, 13006, 10003 },
        { PIC_TFF_RFF, FRAME_B, false, 16009, 13006 },
        { PIC_BFF_RFF, FRAME_B, false, 19012, 16009 },
        { PIC_FRAME, FRAME_P, false, 22015, 19012 },
        { PIC_FRAME_DOUBLING, FRAME_P, false, 25018, 22015 },
        { PIC_FRAME_TRIPLING, FRAME_P, false, 28021, 25018 },
        { PIC_TFF, FRAME_P, false, 31024, 28021 },
    };
    if (!Expect(frames.size() == std::size(expectedFrames), _T("H.264のフレーム数が一致しません"), diagnostic)) return false;
    for (size_t i = 0; i < frames.size(); i++) {
        if (!ExpectVideoFrame(frames[i], expectedFrames[i], i, diagnostic)) return false;
    }
    // クロップ後1440x1080、VUIのtime_scale/2/num_units_in_tickで29.97fps、BT.709
    if (!ExpectVideoFormat(frames[0].format, { 1440, 1080, 1440, 1080, 4, 3, 30000, 1001, 1, false }, diagnostic)) return false;
    return Expect(frames[0].format.format == VS_H264 && frames[0].format.fixedFrameRate,
        _T("H.264のフォーマット種別/固定フレームレートが一致しません"), diagnostic);
}

bool TestH264PanScan(tstring& diagnostic) {
    // pan_scan_rectのオフセットは1/16画素単位の符号付き値。表示幅 = 幅 + (right - left) / 16
    const std::vector<std::vector<uint8_t>> units = {
        MakeH264Unit(H264_AUD_I, true, { MakePicTiming(3, 0, 0), MakePanScanRect(128, -128, 0, 0) }),
        MakeH264Unit(H264_AUD_P, false, { MakePicTiming(3, 2, 0), MakePanScanRect(0, 0, 0, 0) }),
    };
    const std::vector<std::pair<int64_t, int64_t>> timestamps = { { 3003, 0 }, { 6006, 3003 } };
    std::vector<VideoFrameInfo> frames;
    std::vector<int> unitResults;
    if (!ParseVideoUnits(VS_H264, units, timestamps, frames, unitResults, diagnostic)) return false;
    if (!Expect(frames.size() == 2, _T("pan-scanのフレーム数が一致しません"), diagnostic)) return false;
    if (frames[0].format.displayWidth != 1424 || frames[1].format.displayWidth != 1440) {
        diagnostic = strsprintf(_T("pan-scan適用後の表示幅が一致しません: 1枚目=%d/1424, 2枚目=%d/1440"),
            frames[0].format.displayWidth, frames[1].format.displayWidth);
        return false;
    }
    return true;
}

// ADTS: 本体の無音フレーム生成を使って正しいAACフレームを用意する
std::vector<uint8_t> MakeSilentAdts(AUDIO_CHANNELS layout, int samplingFrequencyIndex) {
    size_t length = 0;
    GenerateSilentAdtsFrameForTest(layout, samplingFrequencyIndex, nullptr, 0, &length);
    std::vector<uint8_t> frame(length);
    if (length == 0 || GenerateSilentAdtsFrameForTest(layout, samplingFrequencyIndex, frame.data(), frame.size(), &length) != 0) {
        return {};
    }
    return frame;
}

bool ParseAdts(const std::vector<std::pair<std::vector<uint8_t>, int64_t>>& packets, std::vector<AudioFrameInfoForTest>& frames,
    std::vector<int>& packetResults, tstring& diagnostic) {
    std::vector<AudioPesForTest> inputs;
    for (const auto& packet : packets) inputs.push_back({ packet.first.data(), packet.first.size(), packet.second });
    packetResults.assign(packets.size(), -1);
    size_t count = 0;
    int status = ParseAdtsPacketsForTest(inputs.data(), inputs.size(), packetResults.data(), nullptr, 0, &count);
    if (status == ADTS_PARSER_FOR_TEST_SUCCESS) {
        frames.assign(count, AudioFrameInfoForTest());
        status = ParseAdtsPacketsForTest(inputs.data(), inputs.size(), packetResults.data(), frames.data(), frames.size(), &count);
    }
    if (status != ADTS_PARSER_FOR_TEST_SUCCESS || count != frames.size()) {
        diagnostic = strsprintf(_T("ADTSパーサを実行できません: 結果=%d"), status);
        return false;
    }
    return true;
}

bool ExpectAudioFrame(const AudioFrameInfoForTest& actual, int64_t pts, AUDIO_CHANNELS channels, int sampleRate,
    int codedDataSize, size_t index, tstring& diagnostic) {
    constexpr int AAC_FRAME_SAMPLES = 1024;
    constexpr int DOWNMIX_CHANNELS = 2;
    constexpr int BYTES_PER_SAMPLE = 2;
    if (actual.PTS == pts && actual.channels == channels && actual.sampleRate == sampleRate
        && actual.numSamples == AAC_FRAME_SAMPLES && actual.codedDataSize == codedDataSize
        && actual.decodedDataSize == actual.numDecodedSamples * DOWNMIX_CHANNELS * BYTES_PER_SAMPLE) {
        return true;
    }
    diagnostic = strsprintf(_T("音声フレーム%zu: PTS=%lld/%lld, ch=%d/%d, rate=%d/%d, samples=%d, coded=%d/%d, decoded=%d(%dサンプル)"),
        index, (long long)actual.PTS, (long long)pts, actual.channels, channels, actual.sampleRate, sampleRate,
        actual.numSamples, actual.codedDataSize, codedDataSize, actual.decodedDataSize, actual.numDecodedSamples);
    return false;
}

std::vector<uint8_t> Concat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

bool TestAdtsParser(tstring& diagnostic) {
    constexpr int SFI_48000 = 3;
    constexpr int SFI_44100 = 4;
    constexpr int64_t FRAME_DURATION_48K = 90000 * 1024 / 48000; // 1920
    const auto stereo = MakeSilentAdts(AUDIO_STEREO, SFI_48000);
    const auto mono = MakeSilentAdts(AUDIO_MONO, SFI_44100);
    const auto surround = MakeSilentAdts(AUDIO_32_LFE, SFI_48000);
    if (!Expect(!stereo.empty() && !mono.empty() && !surround.empty(), _T("無音ADTSフレームを生成できません"), diagnostic)) return false;
    const int stereoLength = (int)stereo.size();

    std::vector<AudioFrameInfoForTest> frames;
    std::vector<int> results;
    // 1つのPESに3フレーム: 2番目以降はフレーム長から求めたPTS
    if (!ParseAdts({ { Concat({ stereo, stereo, stereo }), 90000 } }, frames, results, diagnostic)) return false;
    if (!Expect(frames.size() == 3 && results[0] == 1, _T("1つのPESに含まれる3フレームを取得できません"), diagnostic)) return false;
    for (size_t i = 0; i < frames.size(); i++) {
        if (!ExpectAudioFrame(frames[i], 90000 + FRAME_DURATION_48K * (int64_t)i, AUDIO_STEREO, 48000, stereoLength, i, diagnostic)) return false;
    }

    // PES境界がフレームの途中にある場合: 前のPESから続くフレームは前のPTSを引き継ぎ、
    // 新しいPESのPTSはその次のフレームに適用される
    const size_t half = stereo.size() / 2;
    const std::vector<uint8_t> head(stereo.begin(), stereo.begin() + half);
    const std::vector<uint8_t> tail(stereo.begin() + half, stereo.end());
    constexpr int64_t SECOND_PES_PTS = 1000 + FRAME_DURATION_48K * 2 + 5; // 実際のPTSで補正されることを確かめるため少しずらす
    if (!ParseAdts({ { Concat({ stereo, head }), 1000 }, { Concat({ tail, stereo }), SECOND_PES_PTS } }, frames, results, diagnostic)) return false;
    if (!Expect(frames.size() == 3, _T("PES境界をまたぐフレームを取得できません"), diagnostic)) return false;
    if (!ExpectAudioFrame(frames[0], 1000, AUDIO_STEREO, 48000, stereoLength, 0, diagnostic)) return false;
    if (!ExpectAudioFrame(frames[1], 1000 + FRAME_DURATION_48K, AUDIO_STEREO, 48000, stereoLength, 1, diagnostic)) return false;
    if (!ExpectAudioFrame(frames[2], SECOND_PES_PTS, AUDIO_STEREO, 48000, stereoLength, 2, diagnostic)) return false;

    // 同期語を含まない先頭のゴミは読み飛ばす
    const std::vector<uint8_t> garbage = { 0x00, 0x11, 0x22, 0xFF, 0x00, 0x47 };
    if (!ParseAdts({ { Concat({ garbage, stereo }), 3000 } }, frames, results, diagnostic)) return false;
    if (!Expect(frames.size() == 1, _T("先頭のゴミを読み飛ばせません"), diagnostic)) return false;
    if (!ExpectAudioFrame(frames[0], 3000, AUDIO_STEREO, 48000, stereoLength, 0, diagnostic)) return false;

    // チャンネル構成とサンプリング周波数の変化: デコーダを作り直してフレームごとの形式を返す
    if (!ParseAdts({ { stereo, 0 }, { mono, FRAME_DURATION_48K }, { surround, 10000 } }, frames, results, diagnostic)) return false;
    if (!Expect(frames.size() == 3, _T("形式が変化するフレームを取得できません"), diagnostic)) return false;
    if (!ExpectAudioFrame(frames[0], 0, AUDIO_STEREO, 48000, stereoLength, 0, diagnostic)) return false;
    if (!ExpectAudioFrame(frames[1], FRAME_DURATION_48K, AUDIO_MONO, 44100, (int)mono.size(), 1, diagnostic)) return false;
    if (!ExpectAudioFrame(frames[2], 10000, AUDIO_32_LFE, 48000, (int)surround.size(), 2, diagnostic)) return false;

    // フレーム長に満たないデータだけでは出力しない
    if (!ParseAdts({ { head, 0 } }, frames, results, diagnostic)) return false;
    return Expect(frames.empty() && results[0] == 0, _T("不完全なフレームを出力しました"), diagnostic);
}

bool TestAudioPesPipeline(tstring& diagnostic) {
    constexpr int SFI_48000 = 3;
    constexpr int SFI_44100 = 4;
    const auto stereo = MakeSilentAdts(AUDIO_STEREO, SFI_48000);
    const auto mono = MakeSilentAdts(AUDIO_MONO, SFI_44100);
    const auto surround = MakeSilentAdts(AUDIO_32_LFE, SFI_48000);
    const size_t half = stereo.size() / 2;
    const std::vector<uint8_t> head(stereo.begin(), stereo.begin() + half);
    const std::vector<uint8_t> tail(stereo.begin() + half, stereo.end());
    const std::vector<std::vector<uint8_t>> payloads = {
        Concat({ stereo, head }), Concat({ tail, stereo }), mono, surround, stereo,
    };
    std::vector<AudioPesForTest> inputs;
    for (const auto& payload : payloads) inputs.push_back({ payload.data(), payload.size(), 90000 });
    const int status = VerifyAudioPesPipelineForTest(inputs.data(), inputs.size());
    if (status != 0) {
        diagnostic = strsprintf(_T("音声並行処理の一致・所有・順序・終了・例外回収の検証に失敗: %d"), status);
        return false;
    }
    return true;
}

// 音声トラックプラン: テスト専用C ABIの入出力をSTLで扱いやすくまとめる
struct AudioPlanData {
    AudioTrackPlanForTest header;
    std::vector<AudioTrackReferenceForTest> frames;
    tstring name() const { return header.name; }
};

constexpr int AUDIO_FRAME_SAMPLES = 1024;

std::vector<AudioTrackFrameInfoForTest> MakeAudioFrames(std::initializer_list<AUDIO_CHANNELS> layouts) {
    std::vector<AudioTrackFrameInfoForTest> frames;
    for (const auto layout : layouts) frames.push_back({ (int)layout, 48000, AUDIO_FRAME_SAMPLES });
    return frames;
}

// AACの1フレームを48kHzで見た時間 (90kHzクロック)
double AudioDuration(int frames) {
    return (double)frames * AUDIO_FRAME_SAMPLES * 90000.0 / 48000;
}

int PlanAudioTracks(bool merge, const std::vector<std::vector<int>>& tracks,
    const std::vector<AudioTrackFrameInfoForTest>& info, double duration, std::vector<AudioPlanData>& plans) {
    std::vector<int> indices;
    std::vector<size_t> lengths;
    for (const auto& track : tracks) {
        indices.insert(indices.end(), track.begin(), track.end());
        lengths.push_back(track.size());
    }
    size_t planCount = 0, refCount = 0;
    int status = PlanAudioTracksForTest(merge ? 1 : 0, indices.data(), lengths.data(), lengths.size(),
        info.data(), info.size(), duration, nullptr, 0, &planCount, nullptr, 0, &refCount);
    plans.clear();
    if (status != AUDIO_TRACK_PLAN_FOR_TEST_BUFFER_TOO_SMALL) return status;
    std::vector<AudioTrackPlanForTest> headers(planCount);
    std::vector<AudioTrackReferenceForTest> refs(refCount);
    status = PlanAudioTracksForTest(merge ? 1 : 0, indices.data(), lengths.data(), lengths.size(),
        info.data(), info.size(), duration, headers.data(), headers.size(), &planCount, refs.data(), refs.size(), &refCount);
    if (status != AUDIO_TRACK_PLAN_FOR_TEST_SUCCESS) return status;
    size_t offset = 0;
    for (const auto& header : headers) {
        AudioPlanData plan;
        plan.header = header;
        plan.frames.assign(refs.begin() + offset, refs.begin() + offset + header.frameCount);
        offset += header.frameCount;
        plans.push_back(std::move(plan));
    }
    return status;
}

int ValidateAudioPlans(const std::vector<AudioPlanData>& plans, double duration) {
    std::vector<AudioTrackPlanForTest> headers;
    std::vector<AudioTrackReferenceForTest> refs;
    for (const auto& plan : plans) {
        headers.push_back(plan.header);
        headers.back().frameCount = plan.frames.size();
        refs.insert(refs.end(), plan.frames.begin(), plan.frames.end());
    }
    // 空のbufferでもnullptrにならないよう1要素確保する
    refs.push_back({});
    return ValidateAudioTrackPlansForTest(headers.data(), headers.size(), refs.data(), duration);
}

bool ExpectPlanned(bool merge, const std::vector<std::vector<int>>& tracks,
    const std::vector<AudioTrackFrameInfoForTest>& info, double duration, std::vector<AudioPlanData>& plans,
    const TCHAR* message, tstring& diagnostic) {
    const int status = PlanAudioTracks(merge, tracks, info, duration, plans);
    if (status == AUDIO_TRACK_PLAN_FOR_TEST_SUCCESS) return true;
    diagnostic = strsprintf(_T("%s (プラン生成に失敗: %d)"), message, status);
    return false;
}

bool ExpectPlanRejected(bool merge, const std::vector<std::vector<int>>& tracks,
    const std::vector<AudioTrackFrameInfoForTest>& info, double duration, const TCHAR* message, tstring& diagnostic) {
    std::vector<AudioPlanData> plans;
    return Expect(PlanAudioTracks(merge, tracks, info, duration, plans) == AUDIO_TRACK_PLAN_FOR_TEST_REJECTED, message, diagnostic);
}

bool ExpectValidateRejected(const std::vector<AudioPlanData>& plans, double duration, const TCHAR* message, tstring& diagnostic) {
    return Expect(ValidateAudioPlans(plans, duration) == AUDIO_TRACK_PLAN_FOR_TEST_REJECTED, message, diagnostic);
}

bool IsAudioRef(const AudioTrackReferenceForTest& ref, AudioTrackOperation operation, int frameIndex) {
    return ref.operation == (int)operation && ref.frameIndex == frameIndex;
}

bool TestAudioTrackSeparatePlanner(tstring& diagnostic) {
    const auto info = MakeAudioFrames({ AUDIO_STEREO, AUDIO_STEREO, AUDIO_32_LFE, AUDIO_32_LFE, AUDIO_32_LFE, AUDIO_MONO });
    std::vector<AudioPlanData> plans;
    if (!ExpectPlanned(false, { { 0, 2, 3, 4 }, { -1, -1, 5, -1 } }, info, AudioDuration(4), plans, _T("分離プラン"), diagnostic)) return false;
    if (!Expect(plans.size() == 3, _T("変化後のトラック数が不正です"), diagnostic)) return false;
    if (!Expect(plans[0].header.sourceTrack == 0 && plans[0].header.layout == AUDIO_32_LFE && plans[0].name() == _T("Audio0-5.1ch"),
        _T("最長レイアウトが先頭になりません"), diagnostic)) return false;
    if (!Expect(plans[0].header.sampleRate == 48000 && plans[0].header.samplingFrequencyIndex == 3,
        _T("サンプリング周波数のインデックスが不正です"), diagnostic)) return false;
    if (!Expect(plans[0].frames[0].operation == (int)AudioTrackOperation::SILENCE, _T("異なるレイアウトが無音になりません"), diagnostic)) return false;
    if (!Expect(plans[0].frames[1].operation == (int)AudioTrackOperation::COPY && plans[0].frames[1].frameIndex == 2 &&
        plans[0].frames[1].srcLayout == AUDIO_32_LFE && plans[0].frames[1].dstLayout == AUDIO_32_LFE &&
        plans[0].frames[1].dualMonoChannel == -1, _T("5.1chのコピー参照が不正です"), diagnostic)) return false;
    if (!Expect(plans[1].header.layout == AUDIO_STEREO && plans[1].frames[0].frameIndex == 0, _T("ステレオのコピーが不正です"), diagnostic)) return false;
    if (!Expect(plans[2].header.sourceTrack == 1 && plans[2].name() == _T("Audio1-1ch") && plans[2].frames[2].frameIndex == 5,
        _T("途中で出現する音声が欠落しました"), diagnostic)) return false;

    if (!ExpectPlanned(false, { { 2, 3 }, { -1, -1 } }, info, AudioDuration(2), plans, _T("CMカット後の分離プラン"), diagnostic)) return false;
    if (!Expect(plans.size() == 1, _T("CMカット後に消えたトラックが残っています"), diagnostic)) return false;
    if (!ExpectPlanned(false, { { 0, 2 } }, info, AudioDuration(2), plans, _T("同じ時間の分離プラン"), diagnostic)) return false;
    if (!Expect(plans.size() == 2 && plans.front().header.layout == AUDIO_32_LFE, _T("同じ時間のレイアウト順が不正です"), diagnostic)) return false;

    // デュアルモノ混在: 主音声 (lang1) のモノラルが先頭、続いて副音声、最後にステレオ
    const auto dualInfo = MakeAudioFrames({ AUDIO_STEREO, AUDIO_2LANG, AUDIO_2LANG, AUDIO_MONO });
    if (!ExpectPlanned(false, { { 0, 1, 2, 3 } }, dualInfo, AudioDuration(4), plans, _T("デュアルモノ混在プラン"), diagnostic)) return false;
    if (!Expect(plans.size() == 3, _T("デュアルモノ混在のトラック数が不正です"), diagnostic)) return false;
    if (!Expect(plans[0].header.logicalTrack == 0 && plans[0].header.layout == AUDIO_MONO, _T("デュアルモノの主音声順が不正です"), diagnostic)) return false;
    if (!Expect(plans[0].frames[1].dualMonoChannel == 0 && plans[0].frames[3].dualMonoChannel == -1,
        _T("主音声のコピー参照が不正です"), diagnostic)) return false;
    if (!Expect(plans[1].header.logicalTrack == 1 && plans[1].frames[1].dualMonoChannel == 1 &&
        plans[1].frames[0].operation == (int)AudioTrackOperation::SILENCE &&
        plans[1].frames[3].operation == (int)AudioTrackOperation::SILENCE, _T("副音声の無音区間が不正です"), diagnostic)) return false;
    if (!Expect(plans[0].name() == _T("Audio0-lang1-1ch") && plans[1].name() == _T("Audio0-lang2-1ch") &&
        plans[2].name() == _T("Audio0-lang1-2ch"), _T("デュアルモノ混在の言語とレイアウト名が不正です"), diagnostic)) return false;

    std::vector<AudioPlanData> mixed;
    if (!ExpectPlanned(false, { { 0, 1, 2, 3 }, { 0, 1, 2, 3 } }, dualInfo, AudioDuration(4), mixed, _T("複数デュアルモノプラン"), diagnostic)) return false;
    std::vector<tstring> names;
    for (const auto& plan : mixed) {
        if (!Expect(plan.name().find(_T(' ')) == tstring::npos, _T("音声名に空白が含まれています"), diagnostic)) return false;
        if (!Expect(std::find(names.begin(), names.end(), plan.name()) == names.end(), _T("音声名が重複しています"), diagnostic)) return false;
        names.push_back(plan.name());
    }

    // 検証関数: 壊したプランを拒否する (plans はデュアルモノ混在の結果)
    if (!Expect(ValidateAudioPlans(plans, AudioDuration(4)) == AUDIO_TRACK_PLAN_FOR_TEST_SUCCESS, _T("正しいプランが検証で拒否されました"), diagnostic)) return false;
    auto badPlans = plans;
    badPlans[0].header.sampleRate = 0;
    badPlans[0].header.samplingFrequencyIndex = -1;
    if (!ExpectValidateRejected(badPlans, AudioDuration(4), _T("不正なサンプルレートが拒否されませんでした"), diagnostic)) return false;
    badPlans = plans;
    badPlans[0].frames.pop_back();
    if (!ExpectValidateRejected(badPlans, AudioDuration(4), _T("フレーム数の不一致が拒否されませんでした"), diagnostic)) return false;
    badPlans = plans;
    badPlans[0].frames[0].dstLayout = AUDIO_STEREO;
    if (!ExpectValidateRejected(badPlans, AudioDuration(4), _T("出力レイアウトの不一致が拒否されませんでした"), diagnostic)) return false;
    badPlans = plans;
    badPlans[0].frames[1].operation = 99;
    if (!ExpectValidateRejected(badPlans, AudioDuration(4), _T("未定義の操作が拒否されませんでした"), diagnostic)) return false;
    badPlans = plans;
    badPlans[0].frames[1].srcLayout = AUDIO_STEREO;
    if (!ExpectValidateRejected(badPlans, AudioDuration(4), _T("COPYの入力レイアウト不一致が拒否されませんでした"), diagnostic)) return false;

    // 入力の異常系
    if (!ExpectPlanRejected(false, { { 0 }, { 1, 2 } }, info, AudioDuration(1), _T("トラック間のフレーム数不一致が拒否されませんでした"), diagnostic)) return false;
    if (!ExpectPlanRejected(false, { { 99 } }, info, AudioDuration(1), _T("範囲外のフレーム参照が拒否されませんでした"), diagnostic)) return false;
    if (!ExpectPlanRejected(false, { { -2 } }, info, AudioDuration(1), _T("負のフレーム参照が拒否されませんでした"), diagnostic)) return false;
    if (!ExpectPlanRejected(false, { { 0, 1 } }, info, AudioDuration(5), _T("映像時間との差が拒否されませんでした"), diagnostic)) return false;
    auto badInfo = info;
    badInfo[1].sampleRate = 44100;
    if (!ExpectPlanRejected(false, { { 0, 1 } }, badInfo, AudioDuration(2), _T("サンプルレートの混在が拒否されませんでした"), diagnostic)) return false;
    badInfo = info;
    badInfo[0].sampleRate = 12345;
    if (!ExpectPlanRejected(false, { { 0 } }, badInfo, AudioDuration(1), _T("未対応のサンプルレートが拒否されませんでした"), diagnostic)) return false;
    badInfo = info;
    badInfo[0].numSamples = 2048;
    if (!ExpectPlanRejected(false, { { 0 } }, badInfo, AudioDuration(1), _T("AAC-LCでないフレーム長が拒否されませんでした"), diagnostic)) return false;
    badInfo = info;
    badInfo[0].channels = AUDIO_22;
    if (!ExpectPlanRejected(false, { { 0 } }, badInfo, AudioDuration(1), _T("未対応のレイアウトが拒否されませんでした"), diagnostic)) return false;
    if (!ExpectPlanned(false, { { -1, -1 } }, info, AudioDuration(2), plans, _T("全区間無音の分離プラン"), diagnostic)) return false;
    return Expect(plans.empty(), _T("全区間が無音のトラックが残っています"), diagnostic);
}

bool TestAudioTrackMergePlanner(tstring& diagnostic) {
    const auto info = MakeAudioFrames({ AUDIO_STEREO, AUDIO_32_LFE, AUDIO_32_LFE, AUDIO_32_LFE, AUDIO_STEREO, AUDIO_2LANG });
    std::vector<AudioPlanData> plans;
    if (!ExpectPlanned(true, { { 0, 1, 2, -1 }, { -1, 4, -1, -1 } }, info, AudioDuration(4), plans, _T("統合プラン"), diagnostic)) return false;
    if (!Expect(plans.size() == 2 && plans[0].header.layout == AUDIO_32_LFE && plans[0].name() == _T("Audio0") &&
        plans[1].header.sourceTrack == 1 && plans[1].name() == _T("Audio1"), _T("統合の最長レイアウトまたはトラック順が不正です"), diagnostic)) return false;
    const auto& first = plans[0].frames;
    if (!Expect(IsAudioRef(first[0], AudioTrackOperation::CONVERT, 0) && first[0].srcLayout == AUDIO_STEREO && first[0].dstLayout == AUDIO_32_LFE &&
        IsAudioRef(first[1], AudioTrackOperation::COPY, 1) && first[3].operation == (int)AudioTrackOperation::SILENCE,
        _T("統合のCOPY/CONVERT/SILENCE参照が不正です"), diagnostic)) return false;

    if (!ExpectPlanned(true, { { 0, 4, 1 } }, info, AudioDuration(3), plans, _T("ステレオ最長の統合プラン"), diagnostic)) return false;
    if (!Expect(plans[0].header.layout == AUDIO_STEREO && plans[0].frames[2].operation == (int)AudioTrackOperation::CONVERT &&
        plans[0].frames[2].srcLayout == AUDIO_32_LFE && plans[0].frames[2].dstLayout == AUDIO_STEREO,
        _T("ステレオ最長時の5.1ch変換が不正です"), diagnostic)) return false;
    if (!ExpectPlanned(true, { { 0, 1 } }, info, AudioDuration(2), plans, _T("同率の統合プラン"), diagnostic)) return false;
    if (!Expect(plans.size() == 1 && plans[0].header.layout == AUDIO_32_LFE, _T("統合の同率時に多チャンネルを選びません"), diagnostic)) return false;

    if (!ExpectPlanned(true, { { 0, 4, 5 } }, info, AudioDuration(3), plans, _T("デュアルモノ統合プラン"), diagnostic)) return false;
    if (!Expect(plans.size() == 2 && plans[0].header.layout == AUDIO_STEREO && plans[1].header.layout == AUDIO_MONO,
        _T("デュアルモノ論理トラックの統合レイアウトが不正です"), diagnostic)) return false;
    if (!Expect(plans[0].frames[2].operation == (int)AudioTrackOperation::CONVERT && plans[0].frames[2].dualMonoChannel == 0 &&
        plans[0].frames[2].srcLayout == AUDIO_MONO && plans[1].frames[2].operation == (int)AudioTrackOperation::COPY &&
        plans[1].frames[2].dualMonoChannel == 1 && plans[1].frames[0].operation == (int)AudioTrackOperation::SILENCE,
        _T("デュアルモノ統合の主副言語参照が不正です"), diagnostic)) return false;

    if (!ExpectPlanned(true, { { 0, 5, 5 } }, info, AudioDuration(3), plans, _T("モノラル最長の統合プラン"), diagnostic)) return false;
    if (!Expect(plans[0].header.layout == AUDIO_MONO && plans[0].frames[0].operation == (int)AudioTrackOperation::CONVERT &&
        plans[0].frames[1].dualMonoChannel == 0, _T("モノラル最長時のステレオ変換が不正です"), diagnostic)) return false;

    // 検証関数: 壊した統合プランを拒否する
    auto invalid = plans;
    invalid[0].frames[0].srcLayout = AUDIO_MONO;
    if (!ExpectValidateRejected(invalid, AudioDuration(3), _T("CONVERTの入出力が同じレイアウトのプランが拒否されませんでした"), diagnostic)) return false;
    invalid = plans;
    invalid[0].frames[0].frameIndex = -1;
    if (!ExpectValidateRejected(invalid, AudioDuration(3), _T("フレーム参照が負のCONVERTが拒否されませんでした"), diagnostic)) return false;
    invalid = plans;
    invalid[0].frames[0].srcLayout = AUDIO_NONE;
    if (!ExpectValidateRejected(invalid, AudioDuration(3), _T("入力レイアウトが未定義のCONVERTが拒否されませんでした"), diagnostic)) return false;

    if (!ExpectPlanned(true, { { 0 }, { -1 } }, info, AudioDuration(1), plans, _T("CMカット後の統合プラン"), diagnostic)) return false;
    if (!Expect(plans.size() == 1, _T("統合でCMカット後の全無音トラックが残っています"), diagnostic)) return false;
    if (!ExpectPlanned(true, { { -1 } }, info, AudioDuration(1), plans, _T("全区間無音の統合プラン"), diagnostic)) return false;
    return Expect(plans.empty(), _T("統合で全無音トラックが残っています"), diagnostic);
}

// 無音ADTSフレームのバイト列が設計書 (doc/amatsukaze_audio_format.md) の手組み結果と一致すること
bool TestAudioSilentAdtsBytes(tstring& diagnostic) {
    constexpr int SFI_48000 = 3;
    struct Expected { AUDIO_CHANNELS layout; const char* hex; };
    const Expected expected[] = {
        { AUDIO_MONO, "fff14c40017ffc00c80007" },
        { AUDIO_STEREO, "fff14c8001dffc2064000190000e" },
        { AUDIO_32_LFE, "fff14d80039ffc00c800010320000c800011320000c8000306400038" },
    };
    for (const auto& item : expected) {
        const auto actual = MakeSilentAdts(item.layout, SFI_48000);
        std::string hex;
        for (const auto byte : actual) {
            hex += "0123456789abcdef"[byte >> 4];
            hex += "0123456789abcdef"[byte & 15];
        }
        if (hex != item.hex) {
            diagnostic = strsprintf(_T("無音AACのバイト列が設計書と一致しません: レイアウト=%d"), (int)item.layout);
            return false;
        }
    }
    // 全対応レイアウトで生成でき、ADTSヘッダのフレーム長が実バイト数と一致すること
    for (const auto layout : { AUDIO_MONO, AUDIO_STEREO, AUDIO_30, AUDIO_31, AUDIO_32, AUDIO_32_LFE }) {
        const auto frame = MakeSilentAdts(layout, SFI_48000);
        if (!Expect(frame.size() > 7 && frame[0] == 0xFF && (frame[1] & 0xF0) == 0xF0, _T("無音AACのADTS同期語が不正です"), diagnostic)) return false;
        const size_t length = ((frame[3] & 3) << 11) | (frame[4] << 3) | (frame[5] >> 5);
        if (!Expect(length == frame.size(), _T("無音AACのADTSフレーム長が実バイト数と一致しません"), diagnostic)) return false;
    }
    // 異常系: デュアルモノ (単一フレームで表せない) とサンプリング周波数インデックスの範囲外は拒否する
    if (!Expect(MakeSilentAdts(AUDIO_2LANG, SFI_48000).empty(), _T("デュアルモノの無音生成が拒否されませんでした"), diagnostic)) return false;
    return Expect(MakeSilentAdts(AUDIO_MONO, 15).empty(), _T("範囲外のサンプリング周波数インデックスが拒否されませんでした"), diagnostic);
}

// 音声のチャンネル変換 (係数と並び順)。PCMはWAV順のインターリーブで、5.1chはFL, FR, FC, LFE, BL, BR
int ConvertChannels(const std::vector<float>& input, AUDIO_CHANNELS src, AUDIO_CHANNELS dst, std::vector<float>& output) {
    size_t count = 0;
    int status = ConvertAudioChannelsForTest(input.data(), input.size(), src, dst, nullptr, 0, &count);
    output.clear();
    if (status != -2) return status;
    output.resize(count);
    return ConvertAudioChannelsForTest(input.data(), input.size(), src, dst, output.data(), output.size(), &count);
}

bool ExpectConverted(const std::vector<float>& input, AUDIO_CHANNELS src, AUDIO_CHANNELS dst,
    const std::vector<float>& expected, const TCHAR* message, tstring& diagnostic) {
    std::vector<float> actual;
    if (!Expect(ConvertChannels(input, src, dst, actual) == 0 && actual.size() == expected.size(), message, diagnostic)) return false;
    for (size_t i = 0; i < expected.size(); i++) {
        if (!ExpectNear(actual[i], expected[i], 2e-6, message, diagnostic)) return false;
    }
    return true;
}

bool TestAudioChannelConversion(tstring& diagnostic) {
    // 5.1chからステレオへのダウンミックス: (主チャンネル + 0.707 * (FC + サラウンド)) / (1 + 2 * 0.707)
    const double normalizer = 1.0 / (1.0 + 2.0 * 0.707);
    const double surround = 0.707 * normalizer;
    const std::vector<std::vector<float>> impulseExpected = {
        { (float)normalizer, 0 }, { 0, (float)normalizer }, { (float)surround, (float)surround },
        { 0, 0 }, { (float)surround, 0 }, { 0, (float)surround },
    };
    for (int channel = 0; channel < 6; channel++) {
        std::vector<float> impulse(6, 0);
        impulse[channel] = 1;
        if (!ExpectConverted(impulse, AUDIO_32_LFE, AUDIO_STEREO, impulseExpected[channel],
            _T("5.1chからステレオへの係数または並び順が不正です"), diagnostic)) return false;
    }
    if (!ExpectConverted({ 0.25f, -0.5f }, AUDIO_STEREO, AUDIO_32_LFE, { 0.25f, -0.5f, 0, 0, 0, 0 },
        _T("ステレオから5.1chへの配置が不正です"), diagnostic)) return false;
    if (!ExpectConverted({ 0.25f }, AUDIO_MONO, AUDIO_STEREO, { 0.25f, 0.25f },
        _T("モノラルから左右への展開が不正です"), diagnostic)) return false;
    if (!ExpectConverted({ 0.25f, -0.5f }, AUDIO_STEREO, AUDIO_MONO, { -0.125f },
        _T("左右の平均がモノラルになりません"), diagnostic)) return false;
    // モノラルと5.1chの間は、ステレオを経由して変換する
    if (!ExpectConverted({ 0.25f }, AUDIO_MONO, AUDIO_32_LFE, { 0.25f, 0.25f, 0, 0, 0, 0 },
        _T("モノラルから5.1chへの変換が不正です"), diagnostic)) return false;
    if (!ExpectConverted({ 1, 0, 0, 0, 0, 0 }, AUDIO_32_LFE, AUDIO_MONO, { (float)(normalizer / 2) },
        _T("5.1chからモノラルへの変換が不正です"), diagnostic)) return false;
    const std::vector<float> ordered = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    if (!ExpectConverted(ordered, AUDIO_32_LFE, AUDIO_32_LFE, ordered, _T("同じレイアウトで順番が変化しました"), diagnostic)) return false;
    // 複数サンプルでは、サンプルごとに独立して変換する
    if (!ExpectConverted({ 0.25f, -0.5f, 1.0f, 0.5f }, AUDIO_STEREO, AUDIO_MONO, { -0.125f, 0.75f },
        _T("複数サンプルのモノラル化が不正です"), diagnostic)) return false;

    std::vector<float> output;
    if (!Expect(ConvertChannels({ 1, 2, 3 }, AUDIO_STEREO, AUDIO_MONO, output) == -1, _T("チャンネル数で割り切れないPCM長が拒否されませんでした"), diagnostic)) return false;
    if (!Expect(ConvertChannels({ 1 }, AUDIO_MONO, AUDIO_30, output) == -1, _T("未対応の組み合わせが拒否されませんでした"), diagnostic)) return false;
    if (!Expect(ConvertChannels({ 1, 2 }, AUDIO_2LANG, AUDIO_STEREO, output) == -1, _T("デュアルモノの変換が拒否されませんでした"), diagnostic)) return false;
    return Expect(ConvertChannels({}, AUDIO_STEREO, AUDIO_MONO, output) == 0 && output.empty(), _T("空PCMの変換が不正です"), diagnostic);
}

// コマンドライン引数: 解析結果をキー=値のテキストで受け取り、値を比較する
using CommandLineResult = std::map<tstring, tstring>;

int ParseCommandLine(const std::vector<tstring>& args, CommandLineResult& result) {
    std::vector<const TCHAR*> argv = { _T("AmatsukazeTest") };
    for (const auto& arg : args) argv.push_back(arg.c_str());
    result.clear();
    size_t length = 0;
    int status = ParseCommandLineForTest((int)argv.size(), argv.data(), nullptr, 0, &length);
    if (status != -2) return status;
    std::vector<TCHAR> buffer(length + 1);
    status = ParseCommandLineForTest((int)argv.size(), argv.data(), buffer.data(), buffer.size(), &length);
    if (status != 0) return status;
    const tstring text(buffer.data(), length);
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find(_T('\n'), begin);
        if (end == tstring::npos) end = text.size();
        const tstring line = text.substr(begin, end - begin);
        const size_t equal = line.find(_T('='));
        if (equal != tstring::npos) result[line.substr(0, equal)] = line.substr(equal + 1);
        begin = end + 1;
    }
    return 0;
}

bool ExpectCommandLine(const std::vector<tstring>& args, const std::vector<std::pair<tstring, tstring>>& expected,
    const TCHAR* name, tstring& diagnostic) {
    CommandLineResult result;
    const int status = ParseCommandLine(args, result);
    if (status != 0) {
        diagnostic = strsprintf(_T("%s: 引数を解析できません (結果=%d)"), name, status);
        return false;
    }
    for (const auto& item : expected) {
        const auto found = result.find(item.first);
        if (found == result.end() || found->second != item.second) {
            diagnostic = strsprintf(_T("%s: %s の期待値=\"%s\", 実際=\"%s\""), name, item.first.c_str(), item.second.c_str(),
                found == result.end() ? _T("(なし)") : found->second.c_str());
            return false;
        }
    }
    return true;
}

bool ExpectCommandLineStatus(const std::vector<tstring>& args, int expectedStatus, const TCHAR* name, tstring& diagnostic) {
    CommandLineResult result;
    const int status = ParseCommandLine(args, result);
    if (status == expectedStatus) return true;
    diagnostic = strsprintf(_T("%s: 結果の期待値=%d, 実際=%d"), name, expectedStatus, status);
    return false;
}

bool TestCommandLineArguments(tstring& diagnostic) {
    // 探索対象の実行ファイル名はPATHやモジュール位置で変わるため、比較する実行ファイルは存在しない絶対パスを明示指定する。
    // パス区切りは両OSで'/'に正規化される。出力ファイルは .mp4 .mkv .m2ts .ts (大文字小文字不問) だけが除かれ、他の拡張子は残る。
    const tstring encoderOptions = _T("--preset slow --profile main --crf 23 --qcomp 0.7 --vbv-bufsize 10000 --keyint -1 --level 3");
    std::vector<tstring> args = {
        _T("-s"), _T("12345"), _T("-i"), _T("C:\\hoge\\input.ts"), _T("-o"), _T("C:\\oops\\output.mmp4"),
        _T("-w"), _T("C:\\hoge\\"), _T("-et"), _T("x265"), _T("--dump"),
        _T("-e"), _T("D:\\program\\revXXX-x265.exe"), _T("-eo"), encoderOptions,
        _T("-m"), _T("D:\\program\\revXXX-muxer.exe"), _T("-t"), _T("D:\\program\\timelineditro.exe"),
        _T("-j"), _T("JJJJJJJJSON.json"), _T("--mode"), _T("test_parseargs"),
    };
    if (!ExpectCommandLine(args, {
        { _T("service_id"), _T("12345") }, { _T("src"), _T("C:/hoge/input.ts") }, { _T("src_original"), _T("C:/hoge/input.ts") },
        { _T("out_base"), _T("C:/oops/output.mmp4") },
        { _T("encoder"), strsprintf(_T("%d"), (int)ENCODER_X265) }, { _T("encoder_path"), _T("D:/program/revXXX-x265.exe") },
        { _T("encoder_options"), encoderOptions }, { _T("muxer_path"), _T("D:/program/revXXX-muxer.exe") },
        { _T("timelineeditor_path"), _T("D:/program/timelineditro.exe") }, { _T("json"), _T("JJJJJJJJSON.json") },
        { _T("dump"), _T("1") }, { _T("mode"), _T("test_parseargs") }, { _T("nicojk_mask"), _T("0") },
    }, _T("全オプション指定(10進のサービスID)"), diagnostic)) return false;

    // サービスIDの16進指定 (0x6308 = 25352)。先頭が小文字の0xの場合だけ16進として扱う
    args[1] = _T("0x6308");
    if (!ExpectCommandLine(args, { { _T("service_id"), _T("25352") } }, _T("サービスIDの16進指定"), diagnostic)) return false;
    // 旧来の綴り違いの --serivceid も受け付ける
    if (!ExpectCommandLine({ _T("--serivceid"), _T("0x10"), _T("--serviceid"), _T("7"), _T("--mode"), _T("cm"), _T("-i"), _T("a.ts") },
        { { _T("service_id"), _T("7") } }, _T("--serviceid と --serivceid"), diagnostic)) return false;

    // 未知のオプションは拒否する (旧テストは先頭の -s を --ourput に差し替えていた)
    args[0] = _T("--ourput");
    if (!ExpectCommandLineStatus(args, 1, _T("未知のオプション"), diagnostic)) return false;

    // 最小構成の既定値 (実行ファイル位置に依存しない項目のみ)
    if (!ExpectCommandLine({ _T("-i"), _T("a.ts"), _T("-o"), _T("b.mp4") }, {
        { _T("mode"), _T("ts") }, { _T("src"), _T("a.ts") }, { _T("src_original"), _T("a.ts") }, { _T("out_base"), _T("b") },
        { _T("service_id"), _T("-1") }, { _T("encoder"), strsprintf(_T("%d"), (int)ENCODER_X264) },
        { _T("enc_parallel"), _T("1") }, { _T("min_output_duration"), _T("5") }, { _T("max_frames"), _T("9000") },
        { _T("nicojk_mask"), _T("0") }, { _T("dump"), _T("0") }, { _T("json"), _T("") }, { _T("sar_x"), _T("0") }, { _T("sar_y"), _T("0") },
    }, _T("最小構成の既定値"), diagnostic)) return false;
    // 空文字列の引数は無視され、出力は最後の既知拡張子だけが除かれる
    if (!ExpectCommandLine({ _T(""), _T("-i"), _T("a.ts"), _T("-o"), _T("dir/b.c.mkv"), _T("-w"), _T("work"), _T("") },
        { { _T("out_base"), _T("dir/b.c") } }, _T("空引数と拡張子の除去"), diagnostic)) return false;
    if (!ExpectCommandLine({ _T("-i"), _T("a.ts"), _T("-o"), _T("noext") }, { { _T("out_base"), _T("noext") } },
        _T("拡張子のない出力"), diagnostic)) return false;
    if (!ExpectCommandLine({ _T("-i"), _T("a.ts"), _T("-o"), _T("Up.M2TS") }, { { _T("out_base"), _T("Up") } },
        _T("大文字拡張子の除去"), diagnostic)) return false;
    if (!ExpectCommandLine({ _T("--input"), _T("x.ts"), _T("--original-input-file"), _T("orig.ts"), _T("--output"), _T("y.mp4") },
        { { _T("src"), _T("x.ts") }, { _T("src_original"), _T("orig.ts") } }, _T("元入力ファイルの指定"), diagnostic)) return false;

    // モードごとの必須引数
    if (!ExpectCommandLineStatus({ _T("-o"), _T("b.mp4") }, 1, _T("tsモードで入力なし"), diagnostic)) return false;
    if (!ExpectCommandLineStatus({ _T("-i"), _T("a.ts") }, 1, _T("tsモードで出力なし"), diagnostic)) return false;
    if (!ExpectCommandLineStatus({ _T("--mode"), _T("cm") }, 1, _T("cmモードで入力なし"), diagnostic)) return false;
    if (!ExpectCommandLine({ _T("--mode"), _T("cm"), _T("-i"), _T("a.ts") }, { { _T("mode"), _T("cm") } },
        _T("cmモードは出力不要"), diagnostic)) return false;
    if (!ExpectCommandLineStatus({ _T("--mode"), _T("probe_audio") }, 1, _T("probeモードで入力なし"), diagnostic)) return false;

    // 値が欠けた末尾のオプションと、数値でない値
    for (const TCHAR* option : { _T("-i"), _T("-o"), _T("-s"), _T("-e"), _T("--mode"), _T("--logo") }) {
        if (!ExpectCommandLineStatus({ _T("-i"), _T("a.ts"), _T("-o"), _T("b.mp4"), option }, 1,
            strsprintf(_T("値が欠けた末尾の %s"), option).c_str(), diagnostic)) return false;
    }
    // 数値は厳密に解析し、空・末尾のゴミ・範囲外はすべて引数の拒否になる (その他の例外にはならない)
    const std::vector<tstring> numberBase = { _T("-i"), _T("a.ts"), _T("-o"), _T("b.mp4") };
    auto withNumber = [&numberBase](std::initializer_list<tstring> extra) {
        std::vector<tstring> result = numberBase;
        result.insert(result.end(), extra.begin(), extra.end());
        return result;
    };
    for (const TCHAR* value : { _T("abc"), _T("12abc"), _T(""), _T("0x"), _T("0xZZ"), _T("0x-1"), _T("0x0x10"), _T("99999999999"), _T("1.5") }) {
        if (!ExpectCommandLineStatus(withNumber({ _T("-s"), value }), 1, strsprintf(_T("不正なサービスID \"%s\""), value).c_str(), diagnostic)) return false;
    }
    if (!ExpectCommandLine(withNumber({ _T("-s"), _T("0X10") }), { { _T("service_id"), _T("16") } }, _T("大文字0Xの16進サービスID"), diagnostic)) return false;
    if (!ExpectCommandLine(withNumber({ _T("-s"), _T("0xaBc") }), { { _T("service_id"), _T("2748") } }, _T("16進数字の大文字小文字混在"), diagnostic)) return false;
    if (!ExpectCommandLine(withNumber({ _T("-s"), _T("-1") }), { { _T("service_id"), _T("-1") } }, _T("負のサービスID"), diagnostic)) return false;
    for (const TCHAR* option : { _T("--enc-parallel"), _T("--min-output-duration"), _T("-ab"), _T("--tsreplace-startup-preroll"),
        _T("-eb"), _T("--max-fade-length"), _T("--direct-logo-analysis"), _T("--auto-logo-detect"),
        _T("--auto-logo-detect-search-frames"), _T("--auto-logo-detect-div-x"), _T("--auto-logo-detect-div-y"),
        _T("--auto-logo-detect-block-size"), _T("--auto-logo-detect-threshold"), _T("--auto-logo-detect-margin-x"),
        _T("--auto-logo-detect-margin-y"), _T("-om"), _T("--nicojkmask"), _T("--max-frames"), _T("--parallel-logo-analysis") }) {
        for (const TCHAR* value : { _T("abc"), _T("1x"), _T(""), _T("99999999999") }) {
            if (!ExpectCommandLineStatus(withNumber({ option, value }), 1,
                strsprintf(_T("%s の不正値 \"%s\""), option, value).c_str(), diagnostic)) return false;
        }
        if (!ExpectCommandLineStatus(withNumber({ option, _T("1") }), 0, strsprintf(_T("%s の正常値"), option).c_str(), diagnostic)) return false;
    }
    if (!ExpectCommandLine(withNumber({ _T("--parallel-logo-analysis"), _T("auto") }), {}, _T("ロゴ解析並列数のauto"), diagnostic)) return false;

    // 種別を表す値は、未知の名前を警告だけで通さず拒否する (サーバーが渡す綴りは受理する)
    for (const TCHAR* option : { _T("-et"), _T("-eft"), _T("-aet"), _T("--mpeg2decoder"), _T("--h264decoder"), _T("--hevcdecoder") }) {
        if (!ExpectCommandLineStatus(withNumber({ option, _T("unknown") }), 1, strsprintf(_T("%s の未知の値"), option).c_str(), diagnostic)) return false;
    }
    for (const TCHAR* name : { _T("x264"), _T("x265"), _T("QSVEnc"), _T("NVEnc"), _T("VCEEnc"), _T("SVT-AV1"), _T("nvenc"), _T("svt-av1") }) {
        if (!ExpectCommandLineStatus(withNumber({ _T("-et"), name }), 0, strsprintf(_T("-et %s"), name).c_str(), diagnostic)) return false;
    }
    for (const TCHAR* name : { _T("QSVEnc"), _T("NVEnc"), _T("VCEEnc") }) {
        if (!ExpectCommandLineStatus(withNumber({ _T("-eft"), name }), 0, strsprintf(_T("-eft %s"), name).c_str(), diagnostic)) return false;
    }
    for (const TCHAR* name : { _T("neroAac"), _T("qaac"), _T("fdkaac"), _T("opusenc") }) {
        if (!ExpectCommandLineStatus(withNumber({ _T("-aet"), name }), 0, strsprintf(_T("-aet %s"), name).c_str(), diagnostic)) return false;
    }
    for (const TCHAR* option : { _T("--mpeg2decoder"), _T("--h264decoder"), _T("--hevcdecoder") }) {
        for (const TCHAR* name : { _T("default"), _T("QSV"), _T("CUVID") }) {
            if (!ExpectCommandLineStatus(withNumber({ option, name }), 0, strsprintf(_T("%s %s"), option, name).c_str(), diagnostic)) return false;
        }
        if (!ExpectCommandLineStatus(withNumber({ option, _T("vaapi") }), 1, strsprintf(_T("%s vaapi"), option).c_str(), diagnostic)) return false;
    }

    // 値の範囲と書式
    const std::vector<tstring> base = { _T("-i"), _T("a.ts"), _T("-o"), _T("b.mp4") };
    auto with = [&base](std::initializer_list<tstring> extra) {
        std::vector<tstring> result = base;
        result.insert(result.end(), extra.begin(), extra.end());
        return result;
    };
    if (!ExpectCommandLine(with({ _T("--enc-parallel"), _T("3") }), { { _T("enc_parallel"), _T("3") } }, _T("エンコード並列数"), diagnostic)) return false;
    if (!ExpectCommandLineStatus(with({ _T("--enc-parallel"), _T("0") }), 1, _T("並列数0"), diagnostic)) return false;
    if (!ExpectCommandLine(with({ _T("--min-output-duration"), _T("0") }), { { _T("min_output_duration"), _T("5") } },
        _T("最小出力時間0は既定値5へ正規化"), diagnostic)) return false;
    if (!ExpectCommandLineStatus(with({ _T("--min-output-duration"), _T("-1") }), 1, _T("最小出力時間が負"), diagnostic)) return false;
    if (!ExpectCommandLine(with({ _T("--sar"), _T("16:9") }), { { _T("sar_x"), _T("16") }, { _T("sar_y"), _T("9") } }, _T("SAR指定"), diagnostic)) return false;
    if (!ExpectCommandLineStatus(with({ _T("--sar"), _T("16") }), 1, _T("片方だけのSAR"), diagnostic)) return false;
    if (!ExpectCommandLine(with({ _T("--max-frames"), _T("120") }), { { _T("max_frames"), _T("120") } }, _T("最大フレーム数"), diagnostic)) return false;
    if (!ExpectCommandLine(with({ _T("--format"), _T("mkv") }), { { _T("format"), strsprintf(_T("%d"), (int)FORMAT_MKV) } }, _T("出力形式"), diagnostic)) return false;
    if (!ExpectCommandLineStatus(with({ _T("--format"), _T("avi") }), 1, _T("未知の出力形式"), diagnostic)) return false;
    if (!ExpectCommandLineStatus(with({ _T("--audio-format-change"), _T("foo") }), 1, _T("未知の音声フォーマット変更モード"), diagnostic)) return false;
    if (!ExpectCommandLineStatus(with({ _T("--print-prefix"), _T("foo") }), 1, _T("未知の出力接頭辞"), diagnostic)) return false;
    if (!ExpectCommandLineStatus(with({ _T("-et"), _T("x262") }), 1, _T("x262のMP4出力"), diagnostic)) return false;
    if (!ExpectCommandLine(with({ _T("--logo"), _T("l1.lgd"), _T("--logo"), _T("l2\\l3.lgd"), _T("--erase-logo"), _T("e.lgd") }),
        { { _T("logo"), _T("l1.lgd|l2/l3.lgd") }, { _T("erase_logo"), _T("e.lgd") } }, _T("複数のロゴ指定"), diagnostic)) return false;
    // 実況はオプションで有効化したときだけマスクが有効になる
    if (!ExpectCommandLine(with({ _T("--nicojkmask"), _T("3") }), { { _T("nicojk_mask"), _T("0") } }, _T("実況なしのマスク"), diagnostic)) return false;
    if (!ExpectCommandLine(with({ _T("--nicojk"), _T("--nicojkmask"), _T("3") }), { { _T("nicojk_mask"), _T("3") } }, _T("実況ありのマスク"), diagnostic)) return false;
    if (!ExpectCommandLine(with({ _T("--chapter"), _T("--ignore-no-logo"), _T("--subtitles"), _T("--2pass") }),
        { { _T("chapter"), _T("1") }, { _T("subtitles"), _T("1") }, { _T("two_pass"), _T("1") } }, _T("真偽値オプション"), diagnostic)) return false;
    // ロゴ未指定のチャプター生成は拒否する
    return ExpectCommandLineStatus(with({ _T("--chapter") }), 1, _T("ロゴなしのチャプター生成"), diagnostic);
}

// 保持フレームの期待区間を手書きし、境界補正の実装とは独立に判定する。
bool TestCMCutOffset(tstring& diagnostic) {
    struct Scenario {
        const TCHAR* name;
        CMType type;
        int offset;
        std::vector<EncoderZone> cmzones;
        std::vector<EncoderZone> expected;
        std::vector<int> divisions;
    };
    const int maximum = (std::numeric_limits<int>::max)();
    const Scenario scenarios[] = {
        { _T("無効時の従来保持列"), CMTYPE_NONCM, 0, {{10,20},{30,40}}, {{0,10},{20,30},{40,60}}, {} },
        { _T("全CM境界を2フレーム補正"), CMTYPE_NONCM, 2, {{10,20},{30,40}}, {{0,8},{22,28},{42,60}}, {} },
        { _T("短い本編は消失"), CMTYPE_NONCM, 5, {{10,20},{30,40}}, {{0,5},{45,60}}, {} },
        { _T("前後CMだけを補正"), CMTYPE_EDGE_TRIM, 5, {{0,10},{25,30},{50,60}}, {{15,45}}, {} },
        { _T("前後CMなしの内部CMは維持"), CMTYPE_EDGE_TRIM, 5, {{10,20},{30,40}}, {{0,60}}, {} },
        { _T("先頭CMだけ補正"), CMTYPE_NONCM, 5, {{0,10}}, {{15,60}}, {} },
        { _T("末尾CMだけ補正"), CMTYPE_NONCM, 5, {{50,60}}, {{0,45}}, {} },
        { _T("CM出力は補正しない"), CMTYPE_CM, 5, {{10,20},{30,40}}, {{10,20},{30,40}}, {} },
        { _T("全体出力は補正しない"), CMTYPE_BOTH, 5, {{10,20},{30,40}}, {{0,60}}, {} },
        { _T("CMなしは録画全体を保持"), CMTYPE_NONCM, maximum, {}, {{0,60}}, {} },
        { _T("全CMは空出力"), CMTYPE_NONCM, 5, {{0,60}}, {}, {} },
        { _T("前後カットの全CMは空出力"), CMTYPE_EDGE_TRIM, 5, {{0,60}}, {}, {} },
        { _T("大値でもオーバーフローしない"), CMTYPE_NONCM, maximum, {{10,20},{30,40}}, {}, {} },
        { _T("前後カットの短い本編は消失"), CMTYPE_EDGE_TRIM, maximum, {{0,10},{50,60}}, {}, {} },
        { _T("短い先頭本編消失後も内部CMを維持"), CMTYPE_EDGE_TRIM, 5, {{0,10},{13,20},{50,60}}, {{13,45}}, {} },
        { _T("前後の本編消失後も内部CMを維持"), CMTYPE_EDGE_TRIM, maximum, {{0,10},{25,30},{50,60}}, {{25,30}}, {} },
        { _T("本編途中の分割では追加カットしない"), CMTYPE_NONCM, 2,
            {{10,20},{30,40}}, {{0,8},{22,28},{42,60}}, {0,5,25,45,60} },
        { _T("CM境界で分割しても補正する"), CMTYPE_NONCM, 2,
            {{10,20},{30,40}}, {{0,8},{22,28},{42,60}}, {0,10,20,30,40,60} },
        { _T("前後カットは分割点で補正しない"), CMTYPE_EDGE_TRIM, 5,
            {{0,10},{25,30},{50,60}}, {{15,45}}, {0,20,40,60} },
    };
    constexpr int FRAME_COUNT = 60;
    for (const auto& scenario : scenarios) {
        std::vector<int> actual(FRAME_COUNT, -9), expected(FRAME_COUNT, -1);
        for (const auto& range : scenario.expected) {
            for (int frame = range.startFrame; frame < range.endFrame; frame++) {
                const auto& divs = scenario.divisions;
                expected[frame] = divs.empty() ? 0 : (int)(std::upper_bound(divs.begin(), divs.end(), frame) - divs.begin()) - 1;
            }
        }
        const int status = ApplyCMCutOffsetForTest(FRAME_COUNT, scenario.cmzones.data(), scenario.cmzones.size(),
            scenario.divisions.data(), scenario.divisions.size(), scenario.type, scenario.offset, actual.data(), actual.size());
        if (status != 0) {
            diagnostic = strsprintf(_T("%s: CM境界補正を実行できません(結果=%d)"), scenario.name, status);
            return false;
        }
        for (int frame = 0; frame < FRAME_COUNT; frame++) {
            if (actual[frame] != expected[frame]) {
                diagnostic = strsprintf(_T("%s: フレーム%dの出力先期待値=%d、実際=%d"),
                    scenario.name, frame, expected[frame], actual[frame]);
                return false;
            }
        }
    }
    return true;
}

bool TestCMCutOffsetCommandLine(tstring& diagnostic) {
    const std::vector<tstring> base = { _T("-i"), _T("a.ts"), _T("-o"), _T("b.mp4") };
    if (!ExpectCommandLine(base, {{_T("cm_cut_offset_frames"), _T("0")}},
        _T("CMカット点補正の既定値"), diagnostic)) return false;
    for (const TCHAR* value : { _T("1"), _T("5"), _T("2147483647") }) {
        auto args = base;
        args.insert(args.end(), {_T("--cm-cut-offset"), value});
        if (!ExpectCommandLine(args, {{_T("cm_cut_offset_frames"), value}},
            _T("正のCMカット点補正"), diagnostic)) return false;
    }
    for (const TCHAR* value : { _T("0"), _T("-1"), _T("abc"), _T("1x"), _T("1.5"), _T(""), _T("2147483648") }) {
        auto args = base;
        args.insert(args.end(), {_T("--cm-cut-offset"), value});
        if (!ExpectCommandLineStatus(args, 1, strsprintf(_T("CMカット点補正の不正値%s"), value).c_str(), diagnostic)) return false;
    }
    auto args = base;
    args.push_back(_T("--cm-cut-offset"));
    return ExpectCommandLineStatus(args, 1, _T("CMカット点補正の値欠落"), diagnostic);
}

// WAVEヘッダ: 44バイトの全フィールドを仕様どおりに検証する
bool ExpectLe(const std::vector<uint8_t>& bytes, size_t offset, int size, uint64_t expected, const TCHAR* name, tstring& diagnostic) {
    uint64_t actual = 0;
    for (int i = size - 1; i >= 0; i--) actual = (actual << 8) | bytes[offset + i];
    if (actual == expected) return true;
    diagnostic = strsprintf(_T("%s: 期待値=%llu, 実際=%llu"), name, (unsigned long long)expected, (unsigned long long)actual);
    return false;
}

bool ExpectTag(const std::vector<uint8_t>& bytes, size_t offset, const char* tag, const TCHAR* name, tstring& diagnostic) {
    if (std::equal(tag, tag + 4, bytes.begin() + offset)) return true;
    diagnostic = strsprintf(_T("%s のタグが一致しません"), name);
    return false;
}

int MakeWaveHeader(int channels, int sampleRate, int bits, int64_t samples, std::vector<uint8_t>& header) {
    header.assign(64, 0);
    size_t length = 0;
    const int status = WriteWaveHeaderForTest(channels, sampleRate, bits, samples, header.data(), header.size(), &length);
    header.resize(status == 0 ? length : 0);
    return status;
}

bool ExpectWaveHeader(int channels, int sampleRate, int bits, int64_t samples, const TCHAR* name, tstring& diagnostic) {
    std::vector<uint8_t> h;
    if (!Expect(MakeWaveHeader(channels, sampleRate, bits, samples, h) == 0 && h.size() == 44, name, diagnostic)) return false;
    const uint64_t blockAlign = (uint64_t)channels * (bits / 8);
    const uint64_t dataSize = blockAlign * (uint64_t)samples;
    return ExpectTag(h, 0, "RIFF", name, diagnostic) && ExpectLe(h, 4, 4, 36 + dataSize, _T("RIFFサイズ"), diagnostic) &&
        ExpectTag(h, 8, "WAVE", name, diagnostic) && ExpectTag(h, 12, "fmt ", name, diagnostic) &&
        ExpectLe(h, 16, 4, 16, _T("fmtチャンクサイズ"), diagnostic) && ExpectLe(h, 20, 2, 1, _T("PCM形式"), diagnostic) &&
        ExpectLe(h, 22, 2, channels, _T("チャンネル数"), diagnostic) && ExpectLe(h, 24, 4, sampleRate, _T("サンプルレート"), diagnostic) &&
        ExpectLe(h, 28, 4, (uint64_t)sampleRate * blockAlign, _T("バイトレート"), diagnostic) &&
        ExpectLe(h, 32, 2, blockAlign, _T("ブロックアライン"), diagnostic) && ExpectLe(h, 34, 2, bits, _T("量子化ビット数"), diagnostic) &&
        ExpectTag(h, 36, "data", name, diagnostic) && ExpectLe(h, 40, 4, dataSize, _T("dataサイズ"), diagnostic);
}

bool TestWaveHeader(tstring& diagnostic) {
    if (!ExpectWaveHeader(2, 48000, 16, 1000, _T("16bitステレオ48kHz"), diagnostic)) return false;
    if (!ExpectWaveHeader(1, 24000, 8, 7200000, _T("8bitモノラル24kHz"), diagnostic)) return false;
    if (!ExpectWaveHeader(6, 48000, 24, 12345, _T("24bit 5.1ch"), diagnostic)) return false;
    if (!ExpectWaveHeader(1, 16000, 16, 0, _T("サンプル0"), diagnostic)) return false;
    // 全体が2^32-1バイトちょうどまでは書ける。RIFFサイズ欄 (全体-8) は32bitに収まる
    const int64_t maxSamples = ((int64_t(1) << 32) - 1 - 44);
    if (!ExpectWaveHeader(1, 8000, 8, maxSamples, _T("上限ちょうど"), diagnostic)) return false;
    std::vector<uint8_t> h;
    if (!Expect(MakeWaveHeader(1, 8000, 8, maxSamples + 1, h) == -1, _T("上限を1バイト超えるデータが拒否されませんでした"), diagnostic)) return false;
    if (!Expect(MakeWaveHeader(2, 48000, 16, int64_t(1) << 40, h) == -1, _T("巨大なサンプル数が拒否されませんでした"), diagnostic)) return false;
    if (!Expect(MakeWaveHeader(0, 48000, 16, 1, h) == -1, _T("チャンネル数0が拒否されませんでした"), diagnostic)) return false;
    if (!Expect(MakeWaveHeader(2, 0, 16, 1, h) == -1, _T("サンプルレート0が拒否されませんでした"), diagnostic)) return false;
    if (!Expect(MakeWaveHeader(2, 48000, 12, 1, h) == -1, _T("8の倍数でないビット数が拒否されませんでした"), diagnostic)) return false;
    if (!Expect(MakeWaveHeader(2, 48000, 16, -1, h) == -1, _T("負のサンプル数が拒否されませんでした"), diagnostic)) return false;
    size_t length = 0;
    uint8_t small[10];
    return Expect(WriteWaveHeaderForTest(2, 48000, 16, 1, small, sizeof(small), &length) == -2 && length == 44,
        _T("小さい出力バッファが正しく扱われません"), diagnostic);
}

// 無音ADTSをFAADとlibavcodecで復号すると、全サンプルが0になる
bool TestAudioSilentAdtsDecode(tstring& diagnostic) {
    constexpr int SFI_48000 = 3;
    constexpr int FRAMES = 20;
    struct Case { AUDIO_CHANNELS layout; int channels; };
    const Case cases[] = { { AUDIO_MONO, 1 }, { AUDIO_STEREO, 2 }, { AUDIO_30, 3 }, { AUDIO_31, 4 }, { AUDIO_32, 5 }, { AUDIO_32_LFE, 6 } };
    for (const auto& item : cases) {
        SilentAdtsDecodeResultForTest result = {};
        if (!Expect(DecodeSilentAdtsForTest(item.layout, SFI_48000, FRAMES, &result) == 0,
            strsprintf(_T("%dchの無音ADTSを復号できません"), item.channels).c_str(), diagnostic)) return false;
        // 同梱FAADはモノラルをPS対応のため2chへ展開する
        const int faadChannels = item.layout == AUDIO_MONO ? 2 : item.channels;
        if (!Expect(result.faadChannels == faadChannels && result.avChannels == item.channels,
            strsprintf(_T("%dch: 復号チャンネル数が不正 (FAAD=%d, libavcodec=%d)"), item.channels, result.faadChannels, result.avChannels).c_str(), diagnostic)) return false;
        // 復号器の遅延で先頭フレームが出ない場合があるため、1フレーム分の不足までは許す
        const long long faadUnit = 1024LL * faadChannels, avUnit = 1024LL * item.channels;
        if (!Expect(result.faadSamples > 0 && result.faadSamples % faadUnit == 0 && result.faadSamples >= (FRAMES - 1) * faadUnit && result.faadSamples <= FRAMES * faadUnit,
            strsprintf(_T("%dch: FAADのサンプル数が不正 (%lld)"), item.channels, result.faadSamples).c_str(), diagnostic)) return false;
        if (!Expect(result.avSamples > 0 && result.avSamples % avUnit == 0 && result.avSamples >= (FRAMES - 1) * avUnit && result.avSamples <= FRAMES * avUnit,
            strsprintf(_T("%dch: libavcodecのサンプル数が不正 (%lld)"), item.channels, result.avSamples).c_str(), diagnostic)) return false;
        if (!Expect(result.faadMaxAbs == 0, strsprintf(_T("%dch: FAADの復号結果が無音ではありません (最大絶対値=%d)"), item.channels, result.faadMaxAbs).c_str(), diagnostic)) return false;
        if (!Expect(result.avMaxAbs == 0.0, strsprintf(_T("%dch: libavcodecの復号結果が無音ではありません (最大絶対値=%g)"), item.channels, result.avMaxAbs).c_str(), diagnostic)) return false;
    }
    SilentAdtsDecodeResultForTest result = {};
    return Expect(DecodeSilentAdtsForTest(AUDIO_2LANG, SFI_48000, FRAMES, &result) == -1, _T("未対応レイアウトが拒否されませんでした"), diagnostic);
}

// ビット単位でバイト列を組み立てる (本体のBitWriterに依存せず、期待値を独立に作る)
class BitPacker {
public:
    void put(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; i--) {
            if (bitCount % 8 == 0) bytes.push_back(0);
            if ((value >> i) & 1) bytes.back() |= (uint8_t)(0x80 >> (bitCount % 8));
            bitCount++;
        }
    }
    void align() { bitCount = (bitCount + 7) / 8 * 8; }
    std::vector<uint8_t> bytes;
    int bitCount = 0;
};

std::vector<uint8_t> MakeAdtsFrame(int id, int channelConfig, int sfi, const std::vector<uint8_t>& payload) {
    BitPacker header;
    header.put(0xFFF, 12);
    header.put(id, 1);
    header.put(0, 2);
    header.put(1, 1); // protection_absent
    header.put(1, 2); // AAC-LC
    header.put(sfi, 4);
    header.put(0, 1);
    header.put(channelConfig, 3);
    header.put(0, 4); // original, home, copyright x2
    header.put((uint32_t)(7 + payload.size()), 13);
    header.put(0x7FF, 11);
    header.put(0, 2);
    std::vector<uint8_t> frame = header.bytes;
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// 無音のSCE (単一チャンネルエレメント): id(3) tag(4) global_gain(8) と、スペクトル情報を持たない14ビット
void PutSilentSce(BitPacker& packer, int tag) {
    packer.put(0, 3);
    packer.put(tag, 4);
    packer.put(100, 8);
    packer.put(0, 14);
}

// channel_configuration=0 で、PCEが2つのSCEを宣言するデュアルモノの無音フレーム
std::vector<uint8_t> MakeDualMonoSilentAdts(int sfi) {
    BitPacker pce;
    pce.put(5, 3);   // ID_PCE
    pce.put(0, 4);   // element_instance_tag
    pce.put(1, 2);   // object_type (AAC-LC)
    pce.put(sfi, 4);
    pce.put(2, 4);   // 前方のエレメント数
    pce.put(0, 4);   // 側方
    pce.put(0, 4);   // 後方
    pce.put(0, 2);   // LFE
    pce.put(0, 3);   // assoc data
    pce.put(0, 4);   // valid cc
    pce.put(0, 3);   // mono / stereo / matrix mixdown
    for (int e = 0; e < 2; e++) {
        pce.put(0, 1); // is_cpe=0 (SCE)
        pce.put(e, 4);
    }
    pce.align();
    pce.put(0, 8);   // コメントなし
    PutSilentSce(pce, 0);
    PutSilentSce(pce, 1);
    pce.put(7, 3);   // ID_END
    pce.align();
    return MakeAdtsFrame(0, 0, sfi, pce.bytes);
}

// 分離後の1チャンネル分のフレーム: MPEG-2 ID、channel_configuration=1、元のエレメントのタグを保つ
std::vector<uint8_t> MakeSplitMonoFrame(int sfi, int tag) {
    BitPacker payload;
    PutSilentSce(payload, tag);
    payload.put(7, 3);
    payload.align();
    return MakeAdtsFrame(1, 1, sfi, payload.bytes);
}

bool TestDualMonoSplitter(tstring& diagnostic) {
    constexpr int SFI_48000 = 3;
    constexpr int REPEAT = 3;
    const auto dual = MakeDualMonoSilentAdts(SFI_48000);
    std::vector<uint8_t> out0(256), out1(256);
    size_t length0 = 0, length1 = 0;
    int frames0 = 0, frames1 = 0;
    if (!Expect(SplitDualMonoAdtsForTest(dual.data(), dual.size(), REPEAT, out0.data(), out0.size(), &length0, &frames0,
        out1.data(), out1.size(), &length1, &frames1) == 0, _T("デュアルモノの分離に失敗しました"), diagnostic)) return false;
    if (!Expect(frames0 == REPEAT && frames1 == REPEAT, _T("分離後のフレーム数が入力と一致しません"), diagnostic)) return false;
    out0.resize(length0);
    out1.resize(length1);
    // 主音声 (タグ0) と副音声 (タグ1) が、それぞれ1チャンネルのADTSフレームになる
    std::vector<uint8_t> expected0, expected1;
    for (int i = 0; i < REPEAT; i++) {
        const auto f0 = MakeSplitMonoFrame(SFI_48000, 0);
        const auto f1 = MakeSplitMonoFrame(SFI_48000, 1);
        expected0.insert(expected0.end(), f0.begin(), f0.end());
        expected1.insert(expected1.end(), f1.begin(), f1.end());
    }
    if (!Expect(out0 == expected0, _T("主音声のADTSフレームが期待値と一致しません"), diagnostic)) return false;
    if (!Expect(out1 == expected1, _T("副音声のADTSフレームが期待値と一致しません"), diagnostic)) return false;
    // 分離前後の長さ: 入力は2エレメント分、出力は各1エレメント分で、全フレームが7バイトのヘッダを持つ
    if (!Expect(dual.size() > out0.size() / REPEAT, _T("分離後のフレームが入力より大きくなりました"), diagnostic)) return false;
    // 不正入力: ADTSでないデータは拒否する
    const uint8_t garbage[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    size_t l0 = 0, l1 = 0;
    int c0 = 0, c1 = 0;
    uint8_t buffer[64];
    if (!Expect(SplitDualMonoAdtsForTest(garbage, sizeof(garbage), 1, buffer, sizeof(buffer), &l0, &c0, buffer, sizeof(buffer), &l1, &c1) == -1,
        _T("ADTSでないデータが拒否されませんでした"), diagnostic)) return false;
    // 通常のステレオ (エレメントが2つではない) は分離できない
    const auto stereo = MakeSilentAdts(AUDIO_STEREO, SFI_48000);
    if (!Expect(SplitDualMonoAdtsForTest(stereo.data(), stereo.size(), 1, buffer, sizeof(buffer), &l0, &c0, buffer, sizeof(buffer), &l1, &c1) == -1,
        _T("デュアルモノでないフレームが拒否されませんでした"), diagnostic)) return false;
    return Expect(SplitDualMonoAdtsForTest(dual.data(), dual.size(), 1, buffer, 3, &l0, &c0, buffer, sizeof(buffer), &l1, &c1) == -2 && l0 > 3,
        _T("小さい出力バッファが正しく扱われません"), diagnostic);
}

// MakeVFRBitrateZones の性質テスト: 固定seedの乱数入力で、仕様から独立に求められる不変条件を検証する。
// ゾーンは先頭から末尾までを隙間なく覆い、各ゾーンのビットレートは、その範囲の8フレーム単位の
// ビットレート (フレーム間隔から求めた値にCMゾーンで補正をかけたもの) の平均になる。
struct ZoneScenario {
    int frames;
    double fpsNum, fpsDenom;
    int minRun, maxRun;
    double bitrateCM, timeFactor, costLimit;
    int cmZones;
    bool overhang = false; // 映像の範囲外にはみ出すCMゾーン (先頭より前、末尾より後ろ、完全に範囲外、空) を加える
};

bool CheckZoneScenario(const ZoneScenario& scenario, uint32_t seed, const TCHAR* name, tstring& diagnostic) {
    std::mt19937 random(seed);
    const double tick = 1000.0 * scenario.fpsDenom / scenario.fpsNum;
    std::vector<double> timeCodes;
    double elapsed = 0;
    int remain = scenario.frames;
    while (remain > 0) {
        const int run = std::min(remain, scenario.minRun + (int)(random() % (uint32_t)(scenario.maxRun - scenario.minRun + 1)));
        const int ticks = 1 + (int)(random() % 3);
        for (int i = 0; i < run; i++) {
            timeCodes.push_back(elapsed);
            elapsed += tick * ticks;
        }
        remain -= run;
    }
    timeCodes.push_back(elapsed);
    // 重ならないCMゾーン (フレーム範囲内)
    std::vector<VFRBitrateZoneInputForTest> cmzones;
    int position = 0;
    for (int i = 0; i < scenario.cmZones && position < scenario.frames - 16; i++) {
        const int start = position + (int)(random() % 200);
        const int end = std::min(scenario.frames, start + 16 + (int)(random() % 600));
        if (start >= end) break;
        cmzones.push_back({ start, end });
        position = end;
    }
    if (scenario.overhang) {
        const int n = scenario.frames;
        cmzones.push_back({ -50, 20 });
        cmzones.push_back({ n - 30, n + 500 });
        cmzones.push_back({ n + 10, n + 100 });
        cmzones.push_back({ 40, 40 });
    }
    size_t count = 0;
    int status = MakeVFRBitrateZonesForTest(timeCodes.data(), timeCodes.size(), cmzones.data(), cmzones.size(),
        scenario.bitrateCM, (int)scenario.fpsNum, (int)scenario.fpsDenom, scenario.timeFactor, scenario.costLimit, nullptr, 0, &count);
    if (!Expect(status == VFR_BITRATE_ZONES_FOR_TEST_SUCCESS && count > 0, strsprintf(_T("%s: ゾーンを生成できません"), name).c_str(), diagnostic)) return false;
    std::vector<VFRBitrateZoneOutputForTest> zones(count);
    status = MakeVFRBitrateZonesForTest(timeCodes.data(), timeCodes.size(), cmzones.data(), cmzones.size(),
        scenario.bitrateCM, (int)scenario.fpsNum, (int)scenario.fpsDenom, scenario.timeFactor, scenario.costLimit, zones.data(), zones.size(), &count);
    if (!Expect(status == VFR_BITRATE_ZONES_FOR_TEST_SUCCESS && count == zones.size(), strsprintf(_T("%s: ゾーンを取得できません"), name).c_str(), diagnostic)) return false;

    // 独立に求める8フレーム単位のビットレート
    const int unit = 8;
    const int unitCount = (scenario.frames + unit - 1) / unit;
    std::vector<double> units(unitCount);
    for (int i = 0; i < unitCount; i++) {
        const int start = i * unit;
        const int end = std::min(scenario.frames, start + unit);
        const double ratio = (timeCodes[end] - timeCodes[start]) / 1000.0 * scenario.fpsNum / scenario.fpsDenom / (end - start);
        units[i] = (ratio - 1.0) * scenario.timeFactor + 1.0;
    }
    for (const auto& zone : cmzones) {
        // 映像の範囲外にはみ出した部分は切り詰める。端数のユニットは対象にしない
        for (int k = (std::max(0, zone.startFrame) + unit - 1) / unit; k < std::min(scenario.frames, zone.endFrame) / unit; k++) {
            units[k] *= scenario.bitrateCM;
        }
    }
    // 上限判定が末尾ブロックを数えないため、ゾーン数は最大で1000ではなく1001になる (既知。修正はゾーン数の分布を変える)
    if (!Expect(zones.size() <= 1001, strsprintf(_T("%s: ゾーン数が上限を超えています (%zu)"), name, zones.size()).c_str(), diagnostic)) return false;
    if (!Expect(zones.front().startFrame == 0 && zones.back().endFrame == scenario.frames,
        strsprintf(_T("%s: ゾーンが先頭から末尾までを覆っていません"), name).c_str(), diagnostic)) return false;
    for (size_t i = 0; i < zones.size(); i++) {
        const auto& zone = zones[i];
        if (!Expect(zone.startFrame < zone.endFrame && (i == 0 || zone.startFrame == zones[i - 1].endFrame) && zone.startFrame % unit == 0,
            strsprintf(_T("%s: ゾーン%zuの境界が不正です (%d-%d)"), name, i, zone.startFrame, zone.endFrame).c_str(), diagnostic)) return false;
        double sum = 0;
        const int firstUnit = zone.startFrame / unit, lastUnit = (zone.endFrame + unit - 1) / unit;
        for (int k = firstUnit; k < lastUnit; k++) sum += units[k];
        if (!ExpectNear(zone.bitrate, sum / (lastUnit - firstUnit), 1e-9,
            strsprintf(_T("%s: ゾーン%zuのビットレート"), name, i).c_str(), diagnostic)) return false;
        if (!Expect(zone.bitrate > 0, strsprintf(_T("%s: ゾーン%zuのビットレートが正ではありません"), name, i).c_str(), diagnostic)) return false;
    }
    return true;
}

bool TestBitrateZonesProperties(tstring& diagnostic) {
    const ZoneScenario scenarios[] = {
        //  frames   fps                  run       bitrateCM timeFactor costLimit CMゾーン数
        { 1,      60000, 1001, 1, 1,     1.0, 1.0, 0.15, 0 },
        { 7,      60000, 1001, 1, 3,     1.0, 1.0, 0.15, 0 },
        { 8,      30000, 1001, 1, 3,     1.0, 1.0, 0.15, 0 },
        { 9,      30000, 1001, 1, 4,     0.6, 1.0, 0.15, 0 },
        { 100,    60000, 1001, 1, 20,    0.6, 0.25, 0.15, 1 },
        { 1234,   60000, 1001, 5, 60,    0.6, 1.0, 0.15, 3 },
        { 1234,   24000, 1001, 5, 60,    1.5, 1.0, 0.0, 3 },
        { 20000,  60000, 1001, 8, 200,   0.6, 1.0, 1.0, 20 },
        { 20000,  30000, 1001, 1, 3,     0.6, 0.25, 0.15, 20 },
        // 範囲外にはみ出すCMゾーン (先頭より前、末尾より後ろ、完全に範囲外、空)
        { 100,    60000, 1001, 1, 20,    0.6, 1.0, 0.15, 0, true },
        { 1234,   30000, 1001, 5, 60,    1.5, 1.0, 0.15, 2, true },
        { 7,      60000, 1001, 1, 3,     0.6, 1.0, 0.15, 0, true },
        // 1時間級の長さで細かく変化し、上限1000を超えるブロック数から併合される
        { 200000, 60000, 1001, 8, 16,    0.6, 1.0, 0.15, 100 },
    };
    uint32_t seed = 20261012;
    for (size_t i = 0; i < std::size(scenarios); i++, seed += 7) {
        if (!CheckZoneScenario(scenarios[i], seed, strsprintf(_T("シナリオ%zu"), i).c_str(), diagnostic)) return false;
    }
    return true;
}

// MPEG-PS: PsStreamWriterで書き出し、PsStreamVerifierと独立の構文解析の両方で検証する
struct PsPesRecord {
    int64_t pts = -1;
    int64_t dts = -1;
    std::vector<uint8_t> payload;
};

int64_t ReadPesTimestamp(const uint8_t* p) {
    return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) | ((int64_t)(p[2] >> 1) << 15) |
        ((int64_t)p[3] << 7) | (int64_t)(p[4] >> 1);
}

bool ParsePsStream(const std::vector<uint8_t>& ps, std::vector<PsPesRecord> records[2], int& psmCount, tstring& diagnostic) {
    const CRC32 crc;
    size_t pos = 0;
    int64_t previousScr = -1;
    psmCount = 0;
    bool first = true;
    while (pos + 4 <= ps.size()) {
        if (!Expect(ps[pos] == 0 && ps[pos + 1] == 0 && ps[pos + 2] == 1, _T("開始コードがありません"), diagnostic)) return false;
        const uint8_t id = ps[pos + 3];
        if (id == 0xB9) return Expect(pos + 4 == ps.size(), _T("終了コードの後にデータがあります"), diagnostic);
        if (id == 0xBA) {
            if (!Expect(pos + 14 <= ps.size(), _T("パックヘッダが途中で終わっています"), diagnostic)) return false;
            const uint8_t* h = &ps[pos + 4];
            if (!Expect((h[0] & 0xC4) == 0x44 && (h[2] & 4) && (h[4] & 4) && (h[5] & 1), _T("パックヘッダのマーカービットが不正です"), diagnostic)) return false;
            const int64_t base = ((int64_t)((h[0] >> 3) & 7) << 30) | ((int64_t)(h[0] & 3) << 28) | ((int64_t)h[1] << 20) |
                ((int64_t)((h[2] >> 3) & 0x1F) << 15) | ((int64_t)(h[2] & 3) << 13) | ((int64_t)h[3] << 5) | (int64_t)((h[4] >> 3) & 0x1F);
            const int64_t scr = base * 300 + (((h[4] & 3) << 7) | (h[5] >> 1));
            if (!Expect(scr >= previousScr, _T("SCRが逆行しています"), diagnostic)) return false;
            previousScr = scr;
            pos += 14 + (ps[pos + 13] & 7);
            continue;
        }
        if (!Expect(pos + 6 <= ps.size(), _T("PESヘッダが途中で終わっています"), diagnostic)) return false;
        const size_t length = ((size_t)ps[pos + 4] << 8) | ps[pos + 5];
        if (!Expect(pos + 6 + length <= ps.size(), _T("PESパケットが途中で終わっています"), diagnostic)) return false;
        const uint8_t* body = &ps[pos + 6];
        if (id == 0xBC) {
            psmCount++;
            if (!Expect(first, _T("プログラムストリームマップが先頭にありません"), diagnostic)) return false;
            // CRC付きセクションの全体のCRCは0になる
            if (!Expect(crc.calc(&ps[pos], (int)(6 + length), 0xFFFFFFFFu) == 0, _T("プログラムストリームマップのCRCが不正です"), diagnostic)) return false;
            // 単一のストリームマップ: (stream_type, elementary_stream_id) が映像 (MPEG-2=0x02, 0xE0) と音声 (ADTS AAC=0x0F, 0xC0)
            // 本体 (2バイトのフラグ、program_stream_info_length、elementary_stream_map_length、2ストリーム分、CRC) = 18バイト
            if (!Expect(length == 18 && body[6] == 0x02 && body[7] == 0xE0 && body[10] == 0x0F && body[11] == 0xC0,
                _T("プログラムストリームマップの内容が不正です"), diagnostic)) return false;
        } else if (id == 0xE0 || id == 0xC0) {
            const bool video = id == 0xE0;
            const int flags = body[1] >> 6;
            const int headerLength = body[2];
            if (!Expect((body[0] & 0xC0) == 0x80 && length >= 3 + (size_t)headerLength, _T("PESヘッダが不正です"), diagnostic)) return false;
            auto& list = records[video ? 0 : 1];
            if (flags != 0) {
                PsPesRecord record;
                record.pts = ReadPesTimestamp(body + 3);
                if (flags == 3) record.dts = ReadPesTimestamp(body + 8);
                list.push_back(record);
            } else if (!Expect(!list.empty(), _T("タイムスタンプのない先頭PESがあります"), diagnostic)) {
                return false;
            }
            list.back().payload.insert(list.back().payload.end(), body + 3 + headerLength, body + length);
        } else if (!Expect(false, _T("想定外のstream_idです"), diagnostic)) {
            return false;
        }
        first = false;
        pos += 6 + length;
    }
    diagnostic = _T("終了コードがありません");
    return false;
}

bool TestPsWriterRoundTrip(tstring& diagnostic) {
    struct Packet { bool video; int64_t pts, dts; size_t size; };
    // 映像は大きなPES (32000バイトごとに分割される) を含む。音声はPTSのみ
    const Packet packets[] = {
        { true, 90000, 86997, 100 }, { false, 90000, -1, 200 }, { true, 93003, 90000, 40000 }, { false, 91920, -1, 300 },
        { true, 96006, 93003, 70000 }, { false, 93840, -1, 213 }, { true, (int64_t(1) << 33) - 3003, (int64_t(1) << 33) - 6006, 1 },
    };
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<PsWriterInputForTest> inputs;
    for (size_t i = 0; i < std::size(packets); i++) {
        std::vector<uint8_t> payload(packets[i].size);
        for (size_t k = 0; k < payload.size(); k++) payload[k] = (uint8_t)(k * 7 + i * 13 + 1);
        payloads.push_back(std::move(payload));
    }
    for (size_t i = 0; i < std::size(packets); i++) {
        inputs.push_back({ packets[i].video ? 1 : 0, packets[i].pts, packets[i].dts, payloads[i].data(), payloads[i].size() });
    }
    size_t length = 0;
    int status = WritePsStreamForTest(inputs.data(), inputs.size(), nullptr, 0, &length);
    if (!Expect(status == -2 && length > 0, _T("PS出力の必要サイズを取得できません"), diagnostic)) return false;
    std::vector<uint8_t> ps(length);
    status = WritePsStreamForTest(inputs.data(), inputs.size(), ps.data(), ps.size(), &length);
    if (!Expect(status == 0 && length == ps.size(), strsprintf(_T("PsStreamVerifierがPS出力を拒否しました (結果=%d)"), status).c_str(), diagnostic)) return false;

    std::vector<PsPesRecord> records[2];
    int psmCount = 0;
    if (!ParsePsStream(ps, records, psmCount, diagnostic)) return false;
    if (!Expect(psmCount == 1, _T("プログラムストリームマップの数が不正です"), diagnostic)) return false;
    size_t videoIndex = 0, audioIndex = 0;
    if (!Expect(records[0].size() == 4 && records[1].size() == 3, _T("映像・音声のPESの数が入力と一致しません"), diagnostic)) return false;
    for (size_t i = 0; i < std::size(packets); i++) {
        const auto& record = packets[i].video ? records[0][videoIndex++] : records[1][audioIndex++];
        if (!Expect(record.pts == packets[i].pts && record.dts == packets[i].dts, strsprintf(_T("パケット%zuのPTS/DTSが一致しません"), i).c_str(), diagnostic)) return false;
        if (!Expect(record.payload == payloads[i], strsprintf(_T("パケット%zuのペイロードが一致しません"), i).c_str(), diagnostic)) return false;
    }
    // 壊れたストリームは独立の解析でも拒否される (終了コードの欠落)
    std::vector<uint8_t> truncated(ps.begin(), ps.end() - 4);
    std::vector<PsPesRecord> unused[2];
    int unusedCount = 0;
    tstring ignored;
    return Expect(!ParsePsStream(truncated, unused, unusedCount, ignored), _T("終了コードのないPSが受理されました"), diagnostic);
}

// 音声トラックの構築・StreamReformの音声: 一時ディレクトリは自分で作り、終了時に必ず削除する
class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / (std::string("amt_native_test_") + std::to_string(stamp) + "_" + std::to_string(counter++));
        std::error_code error;
        created_ = std::filesystem::create_directories(path_, error);
    }
    ~ScopedTempDirectory() {
        if (created_) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;
    bool ok() const { return created_; }
    tstring path() const { return path_.native(); }
private:
    std::filesystem::path path_;
    bool created_ = false;
};

struct ReformRun {
    AudioReformResultForTest result = {};
    std::vector<int> track0, track1;
    std::vector<int64_t> track0Pts;
};

bool RunAudioReform(int missingFrames, bool cut, int sourcePhaseTicks, int fallback, AUDIO_FORMAT_CHANGE_MODE mode,
    const tstring& directory, ReformRun& run, tstring& diagnostic) {
    constexpr size_t CAPACITY = 1024;
    run = ReformRun();
    run.track0.assign(CAPACITY, -9);
    run.track1.assign(CAPACITY, -9);
    run.track0Pts.assign(CAPACITY, -9);
    AudioReformScenarioForTest scenario = { missingFrames, cut ? 1 : 0, sourcePhaseTicks, fallback, (int)mode, directory.c_str() };
    const int status = RunAudioReformScenarioForTest(&scenario, &run.result, run.track0.data(), run.track0Pts.data(), run.track1.data(), CAPACITY);
    run.track0.resize(std::max(0, run.result.track0Count));
    run.track1.resize(std::max(0, run.result.track1Count));
    run.track0Pts.resize(run.track0.size());
    if (status == 0) return true;
    diagnostic = strsprintf(_T("音声シナリオ(欠落=%d, cut=%d, 位相=%d, fallback=%d, mode=%d)を実行できません: 結果=%d %s"),
        missingFrames, cut ? 1 : 0, sourcePhaseTicks, fallback, (int)mode, status, run.result.message);
    return false;
}

// 旧 AudioTrackStreamReformTest の RunScenario の判定
bool CheckAudioReformScenario(int missingFrames, bool cut, int sourcePhaseTicks, int fallback, AUDIO_FORMAT_CHANGE_MODE mode,
    const tstring& directory, tstring& diagnostic) {
    const tstring name = strsprintf(_T("欠落=%d cut=%d 位相=%d fallback=%d mode=%d"), missingFrames, cut ? 1 : 0, sourcePhaseTicks, fallback, (int)mode);
    ReformRun run;
    if (!RunAudioReform(missingFrames, cut, sourcePhaseTicks, fallback, mode, directory, run, diagnostic)) return false;
    const auto& r = run.result;
    const auto fail = [&](const TCHAR* message) {
        diagnostic = name + _T(": ") + message;
        return false;
    };
    if (fallback != 0 && fallback != 5) {
        return r.numEncoders > 1 ? true : fail(_T("未対応音声がジョブ全体のsplitに戻りません"));
    }
    if (r.outFileCount != 1) return fail(_T("音声変更でファイルが分割されました"));
    if (r.trackCount != 2) return fail(_T("途中で増えた音声トラックが失われました"));
    if (run.track0.size() != run.track1.size()) return fail(_T("音声列の長さが一致しません"));
    if (run.track1.front() != -1 || run.track1.back() != -1) return fail(_T("副音声の先頭と末尾が無音になりません"));
    if (!cut) {
        for (int i = 100; i < 100 + missingFrames; i++) {
            if ((run.track0[i] == -1) != (missingFrames > 5)) return fail(_T("MAX_DUP閾値前後の処理が不正です"));
        }
        if (!(run.track1[210] >= 0 && run.track1[300] == -1 && run.track1[420] >= 0)) return fail(_T("副音声の消失または再出現が不正です"));
    } else if (std::count_if(run.track1.begin(), run.track1.end(), [](int v) { return v >= 0; }) != 50) {
        return fail(_T("CMカット後の副音声の再出現区間が不正です"));
    }
    if (sourcePhaseTicks != 0) {
        // 位相が0.7フレームずれた連続音声を、再選択も欠落もせず全フレーム (1920ティック間隔) 使う
        for (size_t i = 1; i < run.track0.size(); i++) {
            if (!(run.track0[i - 1] >= 0 && run.track0[i] >= 0 && run.track0Pts[i] - run.track0Pts[i - 1] == 1920)) {
                return fail(_T("位相が0.7フレームの連続音声を再選択または欠落しました"));
            }
        }
    }
    if (mode == AFC_MERGE) {
        const int expectedTracks = fallback == 5 ? 3 : 2;
        if (r.planCount != expectedTracks) return fail(_T("統合音声の論理トラック数が不正です"));
        for (int track = 0; track < r.planCount; track++) {
            if (tstring(r.planName[track]) != strsprintf(_T("Audio%d"), track)) return fail(_T("統合音声のトラック名が不正です"));
        }
        if (fallback != 5 && !r.planHasConvert[0]) return fail(_T("異なるレイアウトがCONVERT参照になりませんでした"));
    }
    if (fallback == 5 && r.planCount != 3) return fail(_T("cfg0デュアルモノが主副monoへ展開されませんでした"));
    if (r.validatePlans != 0) return fail(_T("プランの検証が失敗しました"));
    for (int i = 0; i < r.planCount; i++) {
        if (r.planFrameCount[i] != r.track0Count) return fail(_T("プランと参照列の長さが一致しません"));
    }
    if (!(r.pcmFrontFrameIndex == -1 && r.pcmFrontWaveLength == 0)) return fail(_T("無音参照のPCM入力が不正です"));
    return true;
}

bool TestAudioReformSeparate(tstring& diagnostic) {
    return CheckAudioReformScenario(5, false, 0, 0, AFC_SEPARATE, tstring(), diagnostic) &&
        CheckAudioReformScenario(6, false, 0, 0, AFC_SEPARATE, tstring(), diagnostic) &&
        CheckAudioReformScenario(20, true, 0, 0, AFC_SEPARATE, tstring(), diagnostic) &&
        CheckAudioReformScenario(0, false, 1344, 0, AFC_SEPARATE, tstring(), diagnostic);
}

bool TestAudioReformMerge(tstring& diagnostic) {
    return CheckAudioReformScenario(5, false, 0, 0, AFC_MERGE, tstring(), diagnostic) &&
        CheckAudioReformScenario(6, false, 0, 0, AFC_MERGE, tstring(), diagnostic) &&
        CheckAudioReformScenario(20, true, 0, 0, AFC_MERGE, tstring(), diagnostic) &&
        CheckAudioReformScenario(0, false, 1344, 0, AFC_MERGE, tstring(), diagnostic);
}

// 未対応の音声 (周波数混在、非対応レイアウト、AAC Main、PCEのステレオ) があると、ジョブ全体がsplitに戻る
bool TestAudioReformUnsupportedFallback(tstring& diagnostic) {
    ScopedTempDirectory directory;
    if (!Expect(directory.ok(), _T("一時ディレクトリを作成できません"), diagnostic)) return false;
    for (const auto mode : { AFC_SEPARATE, AFC_MERGE }) {
        for (int fallback = 1; fallback <= 4; fallback++) {
            if (!CheckAudioReformScenario(0, false, 0, fallback, mode, directory.path(), diagnostic)) return false;
        }
    }
    return true;
}

// cfg0 (PCE) の2SCE音声は、主副の2本のmonoトラックとして展開される
bool TestAudioReformCfg0DualMono(tstring& diagnostic) {
    ScopedTempDirectory directory;
    if (!Expect(directory.ok(), _T("一時ディレクトリを作成できません"), diagnostic)) return false;
    return CheckAudioReformScenario(0, false, 0, 5, AFC_SEPARATE, directory.path(), diagnostic) &&
        CheckAudioReformScenario(0, false, 0, 5, AFC_MERGE, directory.path(), diagnostic);
}

// BuildAudioTrack の入出力
struct BuiltTracks {
    std::vector<BuildAudioTracksPlanResultForTest> plans;
    std::vector<uint8_t> output;
    int status = 0;
};

BuiltTracks BuildTracks(const std::vector<std::vector<uint8_t>>& frames, const std::vector<int>& channels,
    const std::vector<int>& trackFrames, bool merge, const tstring& directory) {
    std::vector<uint8_t> stream;
    std::vector<size_t> lengths;
    for (const auto& frame : frames) {
        stream.insert(stream.end(), frame.begin(), frame.end());
        lengths.push_back(frame.size());
    }
    BuildAudioTracksInputForTest input = { stream.data(), stream.size(), lengths.data(), channels.data(), frames.size(),
        trackFrames.data(), trackFrames.size(), merge ? 1 : 0, directory.c_str() };
    BuiltTracks built;
    size_t planCount = 0, outputLength = 0;
    built.status = BuildAudioTracksForTest(&input, nullptr, 0, &planCount, nullptr, 0, &outputLength);
    if (built.status != -2 && built.status != 0) return built;
    built.plans.assign(planCount, BuildAudioTracksPlanResultForTest());
    built.output.assign(outputLength, 0);
    built.status = BuildAudioTracksForTest(&input, built.plans.data(), built.plans.size(), &planCount,
        built.output.data(), built.output.size(), &outputLength);
    return built;
}

std::vector<uint8_t> PlanOutput(const BuiltTracks& built, size_t plan) {
    const auto& p = built.plans[plan];
    return std::vector<uint8_t>(built.output.begin() + p.outputOffset, built.output.begin() + p.outputOffset + p.outputLength);
}

// ADTSのフレーム列を (先頭位置, 長さ) に分ける。ヘッダが壊れていれば false
bool SplitAdtsStream(const std::vector<uint8_t>& bytes, std::vector<std::pair<size_t, size_t>>& frames) {
    frames.clear();
    for (size_t position = 0; position < bytes.size();) {
        if (position + 7 > bytes.size() || bytes[position] != 0xFF || (bytes[position + 1] & 0xF0) != 0xF0) return false;
        const size_t length = ((bytes[position + 3] & 3) << 11) | (bytes[position + 4] << 3) | (bytes[position + 5] >> 5);
        if (length < 7 || position + length > bytes.size()) return false;
        frames.push_back({ position, length });
        position += length;
    }
    return true;
}

int AdtsChannelConfig(const std::vector<uint8_t>& bytes, size_t position) {
    return ((bytes[position + 2] & 1) << 2) | (bytes[position + 3] >> 6);
}

int ChannelConfigOfLayout(int layout) {
    switch (layout) {
    case AUDIO_MONO: return 1;
    case AUDIO_STEREO: return 2;
    case AUDIO_32_LFE: return 6;
    default: return -1;
    }
}

// channel_configuration=0 で1つのCPE (ステレオ) を持つPCE付きのフレーム。BuildAudioTrackのCOPYは拒否する
std::vector<uint8_t> MakePceStereoSilentAdts(int sfi) {
    const auto stereo = MakeSilentAdts(AUDIO_STEREO, sfi);
    BitPacker pce;
    pce.put(5, 3);
    pce.put(0, 4);
    pce.put(1, 2);
    pce.put(sfi, 4);
    pce.put(1, 4); // 前方のエレメント数
    pce.put(0, 4);
    pce.put(0, 4);
    pce.put(0, 2);
    pce.put(0, 3);
    pce.put(0, 4);
    pce.put(0, 3);
    pce.put(1, 1); // is_cpe=1
    pce.put(0, 4);
    pce.align();
    pce.put(0, 8);
    std::vector<uint8_t> payload = pce.bytes;
    payload.insert(payload.end(), stereo.begin() + 7, stereo.end()); // CPE + END
    return MakeAdtsFrame(0, 0, sfi, payload);
}

bool TestAudioBuilderCopySilence(tstring& diagnostic) {
    ScopedTempDirectory directory;
    if (!Expect(directory.ok(), _T("一時ディレクトリを作成できません"), diagnostic)) return false;
    const auto frame = MakeSilentAdts(AUDIO_STEREO, 3);
    const auto built = BuildTracks({ frame, frame }, { AUDIO_STEREO, AUDIO_STEREO }, { 0, -1, 1 }, false, directory.path());
    if (!Expect(built.status == 0 && built.plans.size() == 1 && built.plans[0].status == 0, _T("コピーと無音のトラックを構築できません"), diagnostic)) return false;
    // 出力は「コピー」「無音」「コピー」の順。無音フレームは同じレイアウトの無音生成と同じバイト列になる
    std::vector<uint8_t> expected;
    for (int i = 0; i < 3; i++) expected.insert(expected.end(), frame.begin(), frame.end());
    return Expect(PlanOutput(built, 0) == expected, _T("PacketCacheコピーと無音の結合が不正です"), diagnostic);
}

bool TestAudioBuilderDualMono(tstring& diagnostic) {
    constexpr int SFI_48000 = 3;
    ScopedTempDirectory directory;
    if (!Expect(directory.ok(), _T("一時ディレクトリを作成できません"), diagnostic)) return false;
    const auto dual = MakeDualMonoSilentAdts(SFI_48000);
    // ADTSパーサがcfg0の2SCE音声をデュアルモノとして判定する
    std::vector<std::pair<std::vector<uint8_t>, int64_t>> packets;
    for (int i = 0; i < 3; i++) packets.push_back({ dual, (int64_t)i * 1920 });
    std::vector<AudioFrameInfoForTest> parsed;
    std::vector<int> packetResults;
    if (!ParseAdts(packets, parsed, packetResults, diagnostic)) return false;
    if (!Expect(!parsed.empty(), _T("cfg0デュアルモノをADTS parserで読めませんでした"), diagnostic)) return false;
    for (const auto& frame : parsed) {
        if (!Expect(frame.channels == AUDIO_2LANG, _T("PCEの2SCE音声がデュアルモノとして判定されませんでした"), diagnostic)) return false;
    }
    const auto stereo = MakeSilentAdts(AUDIO_STEREO, SFI_48000);
    const auto built = BuildTracks({ dual, stereo, dual }, { AUDIO_2LANG, AUDIO_STEREO, AUDIO_2LANG }, { 0, 1, 2 }, false, directory.path());
    if (!Expect(built.status == 0 && built.plans.size() == 3, _T("デュアルモノ混在の出力トラック数が不正です"), diagnostic)) return false;
    // 主音声mono、副音声mono、ステレオの順。分離後のADTSは、それぞれのレイアウトのチャンネル構成になる
    const int expectedLayouts[] = { AUDIO_MONO, AUDIO_MONO, AUDIO_STEREO };
    for (size_t track = 0; track < built.plans.size(); track++) {
        const auto output = PlanOutput(built, track);
        std::vector<std::pair<size_t, size_t>> frames;
        if (!Expect(built.plans[track].status == 0 && built.plans[track].layout == expectedLayouts[track] && SplitAdtsStream(output, frames) && frames.size() == 3,
            _T("デュアルモノ分離後のトラックを読めません"), diagnostic)) return false;
        for (const auto& item : frames) {
            if (!Expect(AdtsChannelConfig(output, item.first) == ChannelConfigOfLayout(expectedLayouts[track]),
                _T("デュアルモノ分離後のチャンネル構成が不正です"), diagnostic)) return false;
        }
    }
    return true;
}

bool TestAudioBuilderRejectPce(tstring& diagnostic) {
    ScopedTempDirectory directory;
    if (!Expect(directory.ok(), _T("一時ディレクトリを作成できません"), diagnostic)) return false;
    const auto pce = MakePceStereoSilentAdts(3);
    // 前提: cfg0のステレオ (CPE) をADTSパーサはステレオとしては読まない
    const auto built = BuildTracks({ pce }, { AUDIO_STEREO }, { 0 }, false, directory.path());
    if (!Expect(built.status == 0 && built.plans.size() == 1, _T("プランを作成できません"), diagnostic)) return false;
    if (!Expect(built.plans[0].status == 1, _T("Builder直呼びでchannel_config=0が拒否されませんでした"), diagnostic)) return false;
    return Expect(tstring(built.plans[0].message).find(_T("ADTSヘッダ")) != tstring::npos,
        _T("PCE音声がCOPYヘッダ検証で拒否されませんでした"), diagnostic);
}

bool TestAudioBuilderMergeConvert(tstring& diagnostic) {
    constexpr int SFI_48000 = 3;
    ScopedTempDirectory directory;
    if (!Expect(directory.ok(), _T("一時ディレクトリを作成できません"), diagnostic)) return false;
    const auto stereo = MakeSilentAdts(AUDIO_STEREO, SFI_48000);
    const auto surround = MakeSilentAdts(AUDIO_32_LFE, SFI_48000);
    for (const bool upmix : { false, true }) {
        // upmix: ステレオ→5.1chに切り替わる。downmix: 5.1chが最後の1フレームだけ現れる
        const std::vector<int> channels = upmix ? std::vector<int>{ AUDIO_STEREO, AUDIO_32_LFE, AUDIO_32_LFE }
                                                : std::vector<int>{ AUDIO_STEREO, AUDIO_STEREO, AUDIO_32_LFE };
        std::vector<std::vector<uint8_t>> frames;
        for (const int channel : channels) frames.push_back(channel == AUDIO_STEREO ? stereo : surround);
        const auto built = BuildTracks(frames, channels, { 0, 1, 2 }, true, directory.path());
        const int targetLayout = upmix ? AUDIO_32_LFE : AUDIO_STEREO;
        if (!Expect(built.status == 0 && built.plans.size() == 1 && built.plans[0].layout == targetLayout && built.plans[0].status == 0,
            _T("統合変換の目標レイアウトが不正です"), diagnostic)) return false;
        const auto output = PlanOutput(built, 0);
        std::vector<std::pair<size_t, size_t>> adts;
        if (!Expect(SplitAdtsStream(output, adts) && adts.size() == 3, _T("統合変換の出力のフレーム数が入力トラックと一致しません"), diagnostic)) return false;
        for (const auto& item : adts) {
            if (!Expect(AdtsChannelConfig(output, item.first) == ChannelConfigOfLayout(targetLayout),
                _T("統合変換の出力のチャンネル構成が不正です"), diagnostic)) return false;
        }
    }
    return true;
}

// 既知PCM: 固定seedの周波数・位相を持つ正弦波の和 (チャンネルごとに別の周波数)
std::vector<float> MakeKnownPcm(int channels, size_t samplesPerChannel, uint32_t seed, double amplitude, double noise = 0) {
    std::mt19937 random(seed);
    constexpr int PARTIALS = 5;
    constexpr double PI = 3.14159265358979323846;
    std::vector<double> frequency(channels * PARTIALS), phase(channels * PARTIALS);
    for (int i = 0; i < channels * PARTIALS; i++) {
        frequency[i] = 200.0 + (random() % 9000);
        phase[i] = 2.0 * PI * (random() % 10000) / 10000.0;
    }
    std::vector<float> pcm(samplesPerChannel * channels);
    for (size_t n = 0; n < samplesPerChannel; n++) {
        for (int c = 0; c < channels; c++) {
            double v = 0;
            for (int k = 0; k < PARTIALS; k++) {
                v += std::sin(2.0 * PI * frequency[c * PARTIALS + k] * n / 48000.0 + phase[c * PARTIALS + k]);
            }
            double sample = amplitude * v / PARTIALS;
            if (noise > 0) {
                // Box-Muller (std::normal_distributionは実装依存なので使わない)
                const double u1 = ((random() >> 8) + 1.0) / 16777217.0, u2 = (random() >> 8) / 16777216.0;
                sample += noise * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * PI * u2);
            }
            pcm[n * channels + c] = (float)sample;
        }
    }
    return pcm;
}

bool EncodeAdts(const std::vector<float>& pcm, int layout, std::vector<uint8_t>& adts, tstring& diagnostic) {
    const size_t samples = pcm.size() / (layout == AUDIO_MONO ? 1 : layout == AUDIO_STEREO ? 2 : 6);
    size_t length = 0;
    if (!Expect(EncodePcmToAdtsForTest(pcm.data(), samples, layout, nullptr, 0, &length) == -2 && length > 0, _T("AACエンコード結果の大きさを取得できません"), diagnostic)) return false;
    adts.assign(length, 0);
    return Expect(EncodePcmToAdtsForTest(pcm.data(), samples, layout, adts.data(), adts.size(), &length) == 0 && length == adts.size(),
        _T("PCMをAACへエンコードできません"), diagnostic);
}

bool DecodeAdts(const std::vector<uint8_t>& adts, int decoder, std::vector<float>& pcm, int& channels, tstring& diagnostic) {
    size_t count = 0;
    if (!Expect(DecodeAdtsToPcmForTest(adts.data(), adts.size(), decoder, nullptr, 0, &count, &channels) == -2 && count > 0,
        _T("ADTSの復号結果の大きさを取得できません"), diagnostic)) return false;
    pcm.assign(count, 0);
    return Expect(DecodeAdtsToPcmForTest(adts.data(), adts.size(), decoder, pcm.data(), pcm.size(), &count, &channels) == 0,
        _T("ADTSを復号できません"), diagnostic);
}

// FAADとlibavcodecで同じAACを復号した結果の一致。
// 許容誤差は、AAC復号器の適合基準 (ISO/IEC 14496-4) の考え方による。16bit出力のフルアキュラシーでは、
// 参照復号との差のRMSが16bit量子化誤差のRMS (2^-15/√12) 以下、最大誤差が2^-14以下であることが求められる。
bool TestAudioAacDecoderAgreement(tstring& diagnostic) {
    const double RMS_LIMIT = std::pow(2.0, -15) / std::sqrt(12.0);
    const double MAX_LIMIT = std::pow(2.0, -14);
    constexpr int FRAMES = 64;
    const struct { int layout; int channels; } cases[] = { { AUDIO_MONO, 1 }, { AUDIO_STEREO, 2 } };
    for (const auto& item : cases) {
        const tstring name = strsprintf(_T("%dch"), item.channels);
        const auto pcm = MakeKnownPcm(item.channels, (size_t)FRAMES * 1024, 20260928 + item.channels, 0.5);
        std::vector<uint8_t> adts;
        if (!EncodeAdts(pcm, item.layout, adts, diagnostic)) return false;
        std::vector<float> faad, av;
        int faadChannels = 0, avChannels = 0;
        if (!DecodeAdts(adts, 0, faad, faadChannels, diagnostic) || !DecodeAdts(adts, 1, av, avChannels, diagnostic)) return false;
        // libavcodecは全フレーム分を出力し、FAADは先頭1フレーム分の出力を出さない。FAADのモノラルはPS対応で2chへ展開される
        const int expectedFaadChannels = item.channels == 1 ? 2 : item.channels;
        if (!Expect(avChannels == item.channels && faadChannels == expectedFaadChannels, (name + _T(": 復号チャンネル数が不正です")).c_str(), diagnostic)) return false;
        const size_t avFrames = av.size() / avChannels / 1024, faadFrames = faad.size() / faadChannels / 1024;
        if (!Expect(avFrames == FRAMES && faadFrames == FRAMES - 1 && av.size() % (avChannels * 1024) == 0 && faad.size() % (faadChannels * 1024) == 0,
            strsprintf(_T("%s: 復号フレーム数が不正です (libavcodec=%zu, FAAD=%zu)"), name.c_str(), avFrames, faadFrames).c_str(), diagnostic)) return false;
        // 無音の一致で通らないよう、復号結果が十分な振幅を持つことを確かめる
        float peak = 0;
        for (float v : av) peak = std::max(peak, std::abs(v));
        if (!Expect(peak > 0.2f, (name + _T(": 復号結果の振幅が小さすぎます")).c_str(), diagnostic)) return false;
        for (int c = 0; c < expectedFaadChannels; c++) {
            const int avChannel = item.channels == 1 ? 0 : c;
            double squares = 0, maximum = 0;
            const size_t count = faad.size() / faadChannels;
            for (size_t i = 0; i < count; i++) {
                const double d = (double)faad[i * faadChannels + c] - av[(i + 1024) * avChannels + avChannel];
                squares += d * d;
                maximum = std::max(maximum, std::abs(d));
            }
            const double rms = std::sqrt(squares / count);
            if (!Expect(rms <= RMS_LIMIT && maximum <= MAX_LIMIT,
                strsprintf(_T("%s ch%d: 復号結果の差が大きすぎます (RMS=%g/%g, 最大=%g/%g)"), name.c_str(), c, rms, RMS_LIMIT, maximum, MAX_LIMIT).c_str(), diagnostic)) return false;
        }
    }
    return true;
}

// CONVERT (チャンネル数の変換と再エンコード)
struct ConvertSources {
    std::vector<std::vector<uint8_t>> frames[7]; // チャンネル数ごとのADTSフレーム
};

bool MakeConvertSources(ConvertSources& sources, tstring& diagnostic) {
    constexpr size_t SAMPLES = 160 * 1024;
    const struct { int channels; int layout; } layouts[] = { { 1, AUDIO_MONO }, { 2, AUDIO_STEREO }, { 6, AUDIO_32_LFE } };
    for (const auto& item : layouts) {
        // 広帯域の入力: 周期波だけだと相関ピークを誤判定しうるので雑音を加える
        const auto pcm = MakeKnownPcm(item.channels, SAMPLES, 20260928 + item.channels, 0.25, 0.005);
        std::vector<uint8_t> adts;
        std::vector<std::pair<size_t, size_t>> frames;
        if (!EncodeAdts(pcm, item.layout, adts, diagnostic)) return false;
        if (!Expect(SplitAdtsStream(adts, frames) && frames.size() == SAMPLES / 1024, _T("変換元AACのフレーム数が不正です"), diagnostic)) return false;
        for (const auto& f : frames) sources.frames[item.channels].emplace_back(adts.begin() + f.first, adts.begin() + f.first + f.second);
    }
    return true;
}

int LayoutOfChannels(int channels) { return channels == 1 ? AUDIO_MONO : channels == 2 ? AUDIO_STEREO : AUDIO_32_LFE; }

struct ConvertResult {
    int status = 0;
    std::vector<uint8_t> adts;
    tstring message;
};

ConvertResult RunConvert(const std::vector<std::vector<uint8_t>>& frames, const std::vector<int>& frameChannels,
    const std::vector<int64_t>& pts, const std::vector<int>& refIndex, const std::vector<int>& refConvert,
    int dstLayout, int dualMonoChannel, const tstring& directory) {
    std::vector<uint8_t> stream;
    std::vector<size_t> lengths;
    for (const auto& f : frames) {
        stream.insert(stream.end(), f.begin(), f.end());
        lengths.push_back(f.size());
    }
    ConvertAdtsTrackInputForTest input = { stream.data(), stream.size(), lengths.data(), frameChannels.data(),
        pts.empty() ? nullptr : pts.data(), frames.size(), refIndex.data(), refConvert.data(), refIndex.size(),
        dstLayout, dualMonoChannel, directory.c_str() };
    ConvertResult result;
    size_t length = 0;
    TCHAR message[96] = {};
    result.status = ConvertAdtsTrackForTest(&input, nullptr, 0, &length, message, std::size(message));
    if (result.status == -2) {
        result.adts.assign(length, 0);
        result.status = ConvertAdtsTrackForTest(&input, result.adts.data(), result.adts.size(), &length, message, std::size(message));
    }
    result.message = message;
    return result;
}

double MaxAbs(const std::vector<float>& values, size_t begin, size_t end, size_t stride, size_t offset) {
    double maximum = 0;
    for (size_t i = begin; i < end; i++) maximum = std::max(maximum, (double)std::abs(values[i * stride + offset]));
    return maximum;
}

bool ExpectConvertCase(const ConvertSources& sources, int src, int dst, int count, int prefix, int suffix,
    const tstring& directory, tstring& diagnostic) {
    const tstring name = strsprintf(_T("CONVERT %dch→%dch (N=%d 前=%d 後=%d)"), src, dst, count, prefix, suffix);
    std::vector<std::vector<uint8_t>> selected;
    std::vector<int> channels, convert;
    for (int i = 0; i < prefix; i++) { selected.push_back(sources.frames[dst][10 + i]); channels.push_back(LayoutOfChannels(dst)); convert.push_back(0); }
    for (int i = 0; i < count; i++) { selected.push_back(sources.frames[src][20 + i]); channels.push_back(LayoutOfChannels(src)); convert.push_back(1); }
    for (int i = 0; i < suffix; i++) { selected.push_back(sources.frames[dst][30 + i]); channels.push_back(LayoutOfChannels(dst)); convert.push_back(0); }
    std::vector<int> indices(selected.size());
    for (size_t i = 0; i < indices.size(); i++) indices[i] = (int)i;
    const auto result = RunConvert(selected, channels, {}, indices, convert, LayoutOfChannels(dst), -1, directory);
    if (!Expect(result.status == 0, (name + _T(": 変換に失敗しました: ") + result.message).c_str(), diagnostic)) return false;
    std::vector<std::pair<size_t, size_t>> frames;
    if (!Expect(SplitAdtsStream(result.adts, frames) && frames.size() == selected.size(), (name + _T(": 出力のフレーム数が入力トラックと一致しません")).c_str(), diagnostic)) return false;
    // 前後のCOPYはバイト単位で変化しない
    for (size_t i = 0; i < frames.size(); i++) {
        const bool copy = !convert[i];
        const std::vector<uint8_t> actual(result.adts.begin() + frames[i].first, result.adts.begin() + frames[i].first + frames[i].second);
        if (copy && !Expect(actual == selected[i], (name + _T(": COPYのフレームが変化しました")).c_str(), diagnostic)) return false;
    }
    std::vector<float> decoded;
    int decodedChannels = 0;
    if (!DecodeAdts(result.adts, 1, decoded, decodedChannels, diagnostic)) return false;
    if (!Expect(decodedChannels == dst && decoded.size() == selected.size() * 1024 * dst, (name + _T(": 復号後のサンプル数が不正です")).c_str(), diagnostic)) return false;
    if (count < 96) return true;

    // CONVERT元の範囲だけを単独で復号し、変換後の期待信号を求める。両復号器の初期の重なりを除いた内部で位置を比較する
    std::vector<uint8_t> sourceAdts;
    for (int i = 0; i < count; i++) sourceAdts.insert(sourceAdts.end(), selected[prefix + i].begin(), selected[prefix + i].end());
    std::vector<float> reference;
    int referenceChannels = 0;
    if (!DecodeAdts(sourceAdts, 1, reference, referenceChannels, diagnostic)) return false;
    const double normalizer = 1 + 2 * 0.707;
    const size_t total = (size_t)count * 1024;
    const int outputSignals = dst == 1 ? 1 : 2;
    std::vector<std::vector<double>> expected(outputSignals, std::vector<double>(total));
    for (size_t n = 0; n < total; n++) {
        const float* row = &reference[n * src];
        if (src == 6) {
            for (int side = 0; side < 2; side++) expected[side][n] = (row[side] + 0.707 * (row[2] + row[4 + side])) / normalizer;
        } else if (src == 2 && dst == 1) {
            expected[0][n] = (row[0] + row[1]) / 2;
        } else if (src == 1) {
            expected[0][n] = expected[1][n] = row[0];
        } else {
            expected[0][n] = row[0];
            expected[1][n] = row[1];
        }
    }
    const size_t begin = 4 * 1024, end = (size_t)(count - 4) * 1024;
    for (int c = 0; c < outputSignals; c++) {
        std::vector<double> converted(total);
        for (size_t n = 0; n < total; n++) converted[n] = decoded[((size_t)prefix * 1024 + n) * dst + c];
        int bestLag = 0;
        double best = -2;
        for (int lag = -32; lag <= 32; lag++) {
            double ab = 0, aa = 0, bb = 0;
            for (size_t n = begin; n < end; n++) {
                const double a = expected[c][n], b = converted[n + lag];
                ab += a * b; aa += a * a; bb += b * b;
            }
            const double correlation = ab / std::sqrt(aa * bb);
            if (correlation > best) { best = correlation; bestLag = lag; }
        }
        if (!Expect(bestLag == 0, strsprintf(_T("%s ch%d: サンプル位置がずれています (ラグ=%d)"), name.c_str(), c, bestLag).c_str(), diagnostic)) return false;
        if (!Expect(best > 0.9, strsprintf(_T("%s ch%d: 変換後の信号の相関が低すぎます (%g)"), name.c_str(), c, best).c_str(), diagnostic)) return false;
    }
    if (dst == 6) {
        const size_t from = ((size_t)prefix + 4) * 1024, to = ((size_t)prefix + count - 4) * 1024;
        for (int c = 2; c < 6; c++) {
            if (!Expect(MaxAbs(decoded, from, to, 6, c) < 1e-6, strsprintf(_T("%s: 追加チャンネル%dが無音になりません"), name.c_str(), c).c_str(), diagnostic)) return false;
        }
    }
    return true;
}

bool TestAudioConvertPosition(tstring& diagnostic) {
    ScopedTempDirectory directory;
    if (!Expect(directory.ok(), _T("一時ディレクトリを作成できません"), diagnostic)) return false;
    ConvertSources sources;
    if (!MakeConvertSources(sources, diagnostic)) return false;
    const struct { int src, dst; } pairs[] = { { 2, 6 }, { 6, 2 }, { 1, 2 }, { 2, 1 } };
    const struct { int count, prefix, suffix; } shapes[] = { { 1, 0, 0 }, { 2, 0, 0 }, { 96, 0, 0 }, { 1, 8, 8 }, { 2, 8, 8 }, { 96, 8, 8 }, { 12, 0, 8 }, { 12, 8, 0 } };
    for (const auto& p : pairs) {
        for (const auto& s : shapes) {
            if (!ExpectConvertCase(sources, p.src, p.dst, s.count, s.prefix, s.suffix, directory.path(), diagnostic)) return false;
        }
    }
    return true;
}

// cfg0 (PCE) のデュアルモノの無音を、主副それぞれのmonoからステレオへCONVERTしても無音のまま
bool TestAudioConvertDualMono(tstring& diagnostic) {
    ScopedTempDirectory directory;
    if (!Expect(directory.ok(), _T("一時ディレクトリを作成できません"), diagnostic)) return false;
    const auto dual = MakeDualMonoSilentAdts(3);
    for (int language = 0; language < 2; language++) {
        const tstring name = strsprintf(_T("デュアルモノの言語%d"), language + 1);
        const std::vector<std::vector<uint8_t>> frames(12, dual);
        const std::vector<int> channels(12, AUDIO_2LANG), convert(12, 1);
        std::vector<int> indices(12);
        for (int i = 0; i < 12; i++) indices[i] = i;
        const auto result = RunConvert(frames, channels, {}, indices, convert, AUDIO_STEREO, language, directory.path());
        std::vector<std::pair<size_t, size_t>> adts;
        if (!Expect(result.status == 0 && SplitAdtsStream(result.adts, adts) && adts.size() == 12, (name + _T(": 変換結果のフレーム数が不正です")).c_str(), diagnostic)) return false;
        std::vector<float> decoded;
        int decodedChannels = 0;
        if (!DecodeAdts(result.adts, 1, decoded, decodedChannels, diagnostic)) return false;
        if (!Expect(decodedChannels == 2 && decoded.size() == 12 * 1024 * 2 && MaxAbs(decoded, 0, decoded.size(), 1, 0) == 0.0,
            (name + _T(": 無音がゼロになりません")).c_str(), diagnostic)) return false;
    }
    return true;
}

// 100秒の欠落をまたぐと、前の有音の履歴は無音のCONVERTへ漏れない
bool TestAudioConvertLongGap(tstring& diagnostic) {
    ScopedTempDirectory directory;
    if (!Expect(directory.ok(), _T("一時ディレクトリを作成できません"), diagnostic)) return false;
    // 有音の2chフレーム1つと、その後の完全に無音の2chフレーム12個
    std::vector<uint8_t> loudAdts;
    std::vector<std::pair<size_t, size_t>> loudFrames;
    if (!EncodeAdts(MakeKnownPcm(2, 48 * 1024, 20260928, 0.3), AUDIO_STEREO, loudAdts, diagnostic)) return false;
    if (!Expect(SplitAdtsStream(loudAdts, loudFrames) && loudFrames.size() == 48, _T("有音フレームを作れません"), diagnostic)) return false;
    const auto silence = MakeSilentAdts(AUDIO_STEREO, 3);
    std::vector<std::vector<uint8_t>> frames = { std::vector<uint8_t>(loudAdts.begin() + loudFrames[40].first, loudAdts.begin() + loudFrames[40].first + loudFrames[40].second) };
    for (int i = 0; i < 12; i++) frames.push_back(silence);
    const std::vector<int> channels(13, AUDIO_STEREO), convert(12, 1);
    std::vector<int> indices(12);
    for (int i = 0; i < 12; i++) indices[i] = i + 1;
    // 有音フレームの時刻が100秒前 (90kHzで9000000) の場合
    std::vector<int64_t> pts = { 0 };
    for (int i = 0; i < 12; i++) pts.push_back(9000000 + (int64_t)i * 1920);
    const auto separated = RunConvert(frames, channels, pts, indices, convert, AUDIO_32_LFE, -1, directory.path());
    std::vector<std::pair<size_t, size_t>> adts;
    if (!Expect(separated.status == 0 && SplitAdtsStream(separated.adts, adts) && adts.size() == 12, _T("長欠落の変換結果のフレーム数が不正です"), diagnostic)) return false;
    std::vector<float> decoded;
    int decodedChannels = 0;
    if (!DecodeAdts(separated.adts, 1, decoded, decodedChannels, diagnostic)) return false;
    if (!Expect(decodedChannels == 6 && decoded.size() == 12 * 1024 * 6, _T("長欠落の復号後のサンプル数が不正です"), diagnostic)) return false;
    double leaked = 0;
    for (float v : decoded) leaked = std::max(leaked, (double)std::abs(v));
    if (!Expect(leaked == 0.0, strsprintf(_T("100秒前の有音の履歴が無音のCONVERTへ漏れました (最大絶対値=%g)"), leaked).c_str(), diagnostic)) return false;

    // 対照: 時刻が連続していれば有音フレームが履歴として使われ、その影響が出力に出る (上の判定が漏れを検出できる条件であることの確認)
    std::vector<int64_t> contiguous;
    for (int i = 0; i < 13; i++) contiguous.push_back((int64_t)i * 1920);
    const auto joined = RunConvert(frames, channels, contiguous, indices, convert, AUDIO_32_LFE, -1, directory.path());
    std::vector<float> joinedPcm;
    int joinedChannels = 0;
    if (!Expect(joined.status == 0, _T("連続した時刻の変換に失敗しました"), diagnostic) || !DecodeAdts(joined.adts, 1, joinedPcm, joinedChannels, diagnostic)) return false;
    double used = 0;
    for (float v : joinedPcm) used = std::max(used, (double)std::abs(v));
    return Expect(used > 0.0, _T("対照の連続した時刻でも出力が無音のままで、漏れを検出できる条件ではありません"), diagnostic);
}

// TS: パケットとPSIセクションの組み立て
constexpr int TS_PACKET_SIZE = 188;
constexpr int TS_PAYLOAD_SIZE = 184;

class TsStreamBuilder {
public:
    // PSIセクションをpointer_field付きで送出し、最終パケットの残りは0xFFで埋める
    void addSection(int pid, const std::vector<uint8_t>& section) {
        std::vector<uint8_t> payload = { 0x00 };
        payload.insert(payload.end(), section.begin(), section.end());
        for (size_t pos = 0; pos < payload.size(); pos += TS_PAYLOAD_SIZE) {
            const size_t len = std::min<size_t>(TS_PAYLOAD_SIZE, payload.size() - pos);
            std::vector<uint8_t> chunk(payload.begin() + pos, payload.begin() + pos + len);
            chunk.resize(TS_PAYLOAD_SIZE, 0xFF);
            addPacket(pid, pos == 0, chunk, false);
        }
    }
    // PESなどのペイロードを送出し、最終パケットはアダプテーションフィールドで埋める
    void addPayload(int pid, const std::vector<uint8_t>& data) {
        for (size_t pos = 0; pos < data.size(); pos += TS_PAYLOAD_SIZE) {
            const size_t len = std::min<size_t>(TS_PAYLOAD_SIZE, data.size() - pos);
            addPacket(pid, pos == 0, std::vector<uint8_t>(data.begin() + pos, data.begin() + pos + len), true);
        }
    }
    // パケットの通し番号 (ParseTsForTestのclock)
    int64_t packetCount() const { return (int64_t)(bytes.size() / TS_PACKET_SIZE); }
    std::vector<uint8_t> bytes;
private:
    std::map<int, int> continuityCounter;
    void addPacket(int pid, bool unitStart, const std::vector<uint8_t>& payload, bool stuffWithAdaptationField) {
        const int cc = continuityCounter[pid];
        continuityCounter[pid] = (cc + 1) & 0xF;
        const size_t stuffing = TS_PAYLOAD_SIZE - payload.size();
        const bool hasAdaptation = stuffWithAdaptationField && stuffing > 0;
        bytes.push_back(TS_SYNC_BYTE);
        bytes.push_back((uint8_t)((unitStart ? 0x40 : 0) | ((pid >> 8) & 0x1F)));
        bytes.push_back((uint8_t)(pid & 0xFF));
        bytes.push_back((uint8_t)((hasAdaptation ? 0x30 : 0x10) | cc));
        if (hasAdaptation) {
            bytes.push_back((uint8_t)(stuffing - 1)); // adaptation_field_length
            if (stuffing > 1) {
                bytes.push_back(0x00);                // フラグなし
                bytes.insert(bytes.end(), stuffing - 2, 0xFF);
            }
        }
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
};

// section_syntax_indicator=1のセクション (CRC付き)
std::vector<uint8_t> MakeLongSection(int tableId, int tableIdExtension, int version, const std::vector<uint8_t>& body) {
    const int sectionLength = 5 + (int)body.size() + 4;
    std::vector<uint8_t> section = {
        (uint8_t)tableId, (uint8_t)(0xB0 | (sectionLength >> 8)), (uint8_t)(sectionLength & 0xFF),
        (uint8_t)(tableIdExtension >> 8), (uint8_t)(tableIdExtension & 0xFF), (uint8_t)(0xC1 | (version << 1)), 0x00, 0x00,
    };
    section.insert(section.end(), body.begin(), body.end());
    const CRC32 crc;
    const uint32_t value = crc.calc(section.data(), (int)section.size(), 0xFFFFFFFFu);
    section.resize(section.size() + 4);
    write32(section.data() + section.size() - 4, value);
    return section;
}

void AppendPid(std::vector<uint8_t>& out, int reservedHigh, int pid) {
    out.push_back((uint8_t)(reservedHigh | (pid >> 8)));
    out.push_back((uint8_t)(pid & 0xFF));
}

std::vector<uint8_t> MakePat(int tsid, const std::vector<std::pair<int, int>>& programs) {
    std::vector<uint8_t> body;
    for (const auto& [programNumber, pid] : programs) {
        body.push_back((uint8_t)(programNumber >> 8));
        body.push_back((uint8_t)(programNumber & 0xFF));
        AppendPid(body, 0xE0, pid);
    }
    return MakeLongSection(0x00, tsid, 0, body);
}

struct PmtStream {
    int streamType;
    int pid;
    std::vector<uint8_t> descriptors;
};

std::vector<uint8_t> MakePmt(int programNumber, int version, int pcrPid, const std::vector<PmtStream>& streams) {
    std::vector<uint8_t> body;
    AppendPid(body, 0xE0, pcrPid);
    body.insert(body.end(), { 0xF0, 0x00 }); // program_info_length=0
    for (const auto& stream : streams) {
        body.push_back((uint8_t)stream.streamType);
        AppendPid(body, 0xE0, stream.pid);
        AppendPid(body, 0xF0, (int)stream.descriptors.size());
        body.insert(body.end(), stream.descriptors.begin(), stream.descriptors.end());
    }
    return MakeLongSection(0x02, programNumber, version, body);
}

// ストリーム識別記述子 (component_tag)
std::vector<uint8_t> StreamIdentifier(int componentTag) {
    return { 0x52, 0x01, (uint8_t)componentTag };
}

// TDT: MJD(16bit) + 時分秒(BCD)
std::vector<uint8_t> MakeTdt(int mjd16, int hour, int minute, int second) {
    auto bcd = [](int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
    return { 0x70, 0x70, 0x05, (uint8_t)(mjd16 >> 8), (uint8_t)(mjd16 & 0xFF), bcd(hour), bcd(minute), bcd(second) };
}

bool ParseTs(const std::vector<uint8_t>& stream, size_t chunkSize, int serviceIndex,
    std::vector<TsSelectorEventForTest>& events, tstring& diagnostic) {
    std::vector<const uint8_t*> chunks;
    std::vector<size_t> lengths;
    for (size_t pos = 0; pos < stream.size(); pos += chunkSize) {
        chunks.push_back(stream.data() + pos);
        lengths.push_back(std::min(chunkSize, stream.size() - pos));
    }
    size_t count = 0;
    int status = ParseTsForTest(chunks.data(), lengths.data(), chunks.size(), serviceIndex, nullptr, 0, &count);
    if (status == TS_SELECTOR_FOR_TEST_SUCCESS) {
        events.assign(count, TsSelectorEventForTest());
        status = ParseTsForTest(chunks.data(), lengths.data(), chunks.size(), serviceIndex, events.data(), events.size(), &count);
    }
    if (status != TS_SELECTOR_FOR_TEST_SUCCESS || count != events.size()) {
        diagnostic = strsprintf(_T("TS選択器を実行できません: 結果=%d"), status);
        return false;
    }
    return true;
}

tstring DescribeTsEvents(const std::vector<TsSelectorEventForTest>& events) {
    tstring text;
    for (const auto& e : events) {
        text += strsprintf(_T(" [type=%d pid=0x%x idx=%d clock=%lld v=%d,%d,%d,%d,%d,%d]"), e.type, e.pid, e.index, (long long)e.clock,
            e.values[0], e.values[1], e.values[2], e.values[3], e.values[4], e.values[5]);
    }
    return text;
}

bool ExpectTsEvents(const std::vector<TsSelectorEventForTest>& actual, const std::vector<TsSelectorEventForTest>& expected,
    const TCHAR* name, tstring& diagnostic) {
    bool matched = actual.size() == expected.size();
    for (size_t i = 0; matched && i < actual.size(); i++) {
        const auto& a = actual[i];
        const auto& e = expected[i];
        matched = a.type == e.type && a.pid == e.pid && a.index == e.index && a.clock == e.clock
            && std::equal(std::begin(a.values), std::end(a.values), std::begin(e.values));
    }
    if (matched) return true;
    diagnostic = tstring(name) + _T(": 期待=") + DescribeTsEvents(expected) + _T(" / 実際=") + DescribeTsEvents(actual);
    return false;
}

TsSelectorEventForTest TsEvent(int type, int pid, int index, int64_t clock, std::initializer_list<int> values) {
    TsSelectorEventForTest e = {};
    e.type = type;
    e.pid = pid;
    e.index = index;
    e.clock = clock;
    std::copy(values.begin(), values.end(), e.values);
    return e;
}

bool TestTsSelector(tstring& diagnostic) {
    constexpr int TSID = 0x7FE0;
    constexpr int SID_MAIN = 0x0400;
    constexpr int SID_SUB = 0x0401;
    constexpr int PMT_PID_MAIN = 0x01F0;
    constexpr int PMT_PID_SUB = 0x01F1;
    constexpr int PCR_PID = 0x01FF;
    constexpr int VIDEO_PID = 0x0111;
    constexpr int AUDIO1_PID = 0x0112;
    constexpr int AUDIO2_PID = 0x0113;
    constexpr int CAPTION_PID = 0x0130;
    constexpr int DATA_PID = 0x0138;
    constexpr int TDT_PID = 0x0014;
    constexpr int STREAM_TYPE_MPEG2 = 0x02;
    constexpr int STREAM_TYPE_AAC = 0x0F;
    constexpr int STREAM_TYPE_PES_PRIVATE = 0x06;
    constexpr int STREAM_TYPE_DSMCC = 0x0D;
    constexpr int COMPONENT_TAG_CAPTION = 0x30;
    constexpr int COMPONENT_TAG_DATA = 0x40;
    constexpr int MJD_2026_10_05 = 61318;
    constexpr int MJD16_2040_01_01 = 66154 & 0xFFFF;

    // データ放送ESに長い記述子を付け、PMTを2パケットにまたがらせる
    std::vector<uint8_t> longDescriptor = { 0xC0, 200 };
    longDescriptor.resize(2 + 200, 0x55);
    const std::vector<PmtStream> streams = {
        { STREAM_TYPE_MPEG2, VIDEO_PID, StreamIdentifier(0x00) },
        { STREAM_TYPE_AAC, AUDIO1_PID, StreamIdentifier(0x10) },
        { STREAM_TYPE_AAC, AUDIO2_PID, StreamIdentifier(0x11) },
        { STREAM_TYPE_PES_PRIVATE, CAPTION_PID, StreamIdentifier(COMPONENT_TAG_CAPTION) },
        { STREAM_TYPE_PES_PRIVATE, DATA_PID, Concat({ StreamIdentifier(COMPONENT_TAG_DATA), longDescriptor }) },
        { STREAM_TYPE_DSMCC, 0x0150, {} },
    };
    const std::vector<uint8_t> payload(300, 0xAB);

    TsStreamBuilder ts;
    ts.addSection(0x0000, MakePat(TSID, { { 0, 0x0010 }, { SID_MAIN, PMT_PID_MAIN }, { SID_SUB, PMT_PID_SUB } }));
    // CRCが壊れたPMTは無視される
    auto brokenPmt = MakePmt(SID_MAIN, 0, PCR_PID, streams);
    brokenPmt[brokenPmt.size() - 1] ^= 0x01;
    ts.addSection(PMT_PID_MAIN, brokenPmt);
    ts.addSection(PMT_PID_MAIN, MakePmt(SID_MAIN, 0, PCR_PID, streams));
    // 別サービスのPMTは選択されていないので処理されない
    ts.addSection(PMT_PID_SUB, MakePmt(SID_SUB, 0, PCR_PID, { { STREAM_TYPE_MPEG2, 0x0211, {} } }));
    // 映像の最初のパケットが来るまで、音声は振り分けられない
    ts.addPayload(AUDIO1_PID, payload);
    const int64_t videoClock = ts.packetCount();
    ts.addPayload(VIDEO_PID, payload);
    const int64_t audioClock = ts.packetCount();
    ts.addPayload(AUDIO1_PID, payload);
    ts.addPayload(AUDIO2_PID, payload);
    ts.addPayload(CAPTION_PID, payload);
    ts.addPayload(DATA_PID, payload);
    const int64_t tdtClock = ts.packetCount();
    ts.addSection(TDT_PID, MakeTdt(MJD_2026_10_05, 22, 15, 30));
    // PMT更新で音声2を削除すると、そのPIDは振り分けられなくなる
    const auto streamsWithoutAudio2 = std::vector<PmtStream>{ streams[0], streams[1], streams[3] };
    ts.addSection(PMT_PID_MAIN, MakePmt(SID_MAIN, 1, PCR_PID, streamsWithoutAudio2));
    const int64_t afterUpdateClock = ts.packetCount();
    ts.addPayload(AUDIO2_PID, payload);
    ts.addPayload(AUDIO1_PID, payload);
    // MJDの16bitが一周した2038年以降の日付
    const int64_t tdt2040Clock = ts.packetCount();
    ts.addSection(TDT_PID, MakeTdt(MJD16_2040_01_01, 0, 0, 0));
    // 同期探索のため末尾に8パケット以上の空パケットを置く
    for (int i = 0; i < 8; i++) ts.addPayload(0x1FFF, std::vector<uint8_t>(TS_PAYLOAD_SIZE, 0xFF));

    using E = TsSelectorEventForTest;
    const std::vector<E> expected = {
        TsEvent(TS_SELECTOR_EVENT_PID_SELECT, -1, -1, -1, { TSID, 2, SID_MAIN, SID_SUB }),
        TsEvent(TS_SELECTOR_EVENT_PMT_UPDATED, -1, -1, -1, { PCR_PID }),
        TsEvent(TS_SELECTOR_EVENT_PID_TABLE, -1, -1, -1, { STREAM_TYPE_MPEG2, VIDEO_PID, 2, AUDIO1_PID, AUDIO2_PID, CAPTION_PID }),
        TsEvent(TS_SELECTOR_EVENT_VIDEO_PACKET, VIDEO_PID, -1, videoClock, { 1, 0 }),
        TsEvent(TS_SELECTOR_EVENT_VIDEO_PACKET, VIDEO_PID, -1, videoClock + 1, { 0, 1 }),
        TsEvent(TS_SELECTOR_EVENT_AUDIO_PACKET, AUDIO1_PID, 0, audioClock, { 1, 2 }),
        TsEvent(TS_SELECTOR_EVENT_AUDIO_PACKET, AUDIO1_PID, 0, audioClock + 1, { 0, 3 }),
        TsEvent(TS_SELECTOR_EVENT_AUDIO_PACKET, AUDIO2_PID, 1, audioClock + 2, { 1, 0 }),
        TsEvent(TS_SELECTOR_EVENT_AUDIO_PACKET, AUDIO2_PID, 1, audioClock + 3, { 0, 1 }),
        TsEvent(TS_SELECTOR_EVENT_CAPTION_PACKET, CAPTION_PID, -1, audioClock + 4, { 1, 0 }),
        TsEvent(TS_SELECTOR_EVENT_CAPTION_PACKET, CAPTION_PID, -1, audioClock + 5, { 0, 1 }),
        TsEvent(TS_SELECTOR_EVENT_TIME, -1, -1, tdtClock, { 2026, 10, 5, 22, 15, 30 }),
        TsEvent(TS_SELECTOR_EVENT_PMT_UPDATED, -1, -1, -1, { PCR_PID }),
        TsEvent(TS_SELECTOR_EVENT_PID_TABLE, -1, -1, -1, { STREAM_TYPE_MPEG2, VIDEO_PID, 1, AUDIO1_PID, -1, CAPTION_PID }),
        TsEvent(TS_SELECTOR_EVENT_AUDIO_PACKET, AUDIO1_PID, 0, afterUpdateClock + 2, { 1, 4 }),
        TsEvent(TS_SELECTOR_EVENT_AUDIO_PACKET, AUDIO1_PID, 0, afterUpdateClock + 3, { 0, 5 }),
        TsEvent(TS_SELECTOR_EVENT_TIME, -1, -1, tdt2040Clock, { 2040, 1, 1, 0, 0, 0 }),
    };
    std::vector<E> events;
    if (!ParseTs(ts.bytes, ts.bytes.size(), 0, events, diagnostic)) return false;
    if (!ExpectTsEvents(events, expected, _T("一括入力"), diagnostic)) return false;

    // 先頭のゴミと半端な分割入力でも同じ結果になる (クロックはパケット番号なので変わらない)
    std::vector<uint8_t> withGarbage = { 0x47, 0x00, 0x11, 0x47, 0x22, 0x33, 0x44 };
    withGarbage.insert(withGarbage.end(), ts.bytes.begin(), ts.bytes.end());
    if (!ParseTs(withGarbage, 97, 0, events, diagnostic)) return false;
    return ExpectTsEvents(events, expected, _T("ゴミ付き分割入力"), diagnostic);
}

bool DecodeArib(const std::vector<uint8_t>& data, std::wstring& decoded) {
    size_t length = 0;
    int status = DecodeAribStringForTest(data.data(), data.size(), nullptr, 0, &length);
    if (status != 0 && status != -2) return false;
    decoded.assign(length, L'\0');
    if (length == 0) return true;
    return DecodeAribStringForTest(data.data(), data.size(), &decoded[0], decoded.size(), &length) == 0;
}

bool TestAribString(tstring& diagnostic) {
    struct TestCase {
        const TCHAR* name;
        std::vector<uint8_t> data;
        const wchar_t* expected;
    };
    // 初期状態はG0=漢字(GL), G1=英数, G2=ひらがな(GR), G3=カタカナ
    const TestCase testCases[] = {
        { _T("空文字列"), {}, L"" },
        { _T("漢字(GL)"), { 0x48, 0x56, 0x41, 0x48 }, L"番組" },
        { _T("ひらがな(GR)"), { 0xA2, 0xA4 }, L"あい" },
        { _T("カタカナ(SS3)"), { 0x1D, 0x22, 0x1D, 0x24 }, L"アイ" },
        { _T("英数字(LS1/LS0)"), { 0x0E, 0x41, 0x42, 0x31, 0x0F, 0x48, 0x56 }, L"ＡＢ１番" },
        { _T("改行(APR)"), { 0x48, 0x56, 0x0D, 0x41, 0x48 }, L"番\r\n組" },
        { _T("番組名と追加記号"), { 0x45, 0x37, 0x35, 0x24, 0x4D, 0x3D, 0x4A, 0x73, 0x7A, 0x56 }, L"天気予報[字]" },
        // 追加記号は2バイトから3文字以上に展開されるため、入力バイト数より出力が長くなる
        { _T("追加記号の連続"), { 0x7A, 0x56, 0x7A, 0x50, 0x7A, 0x56, 0x7A, 0x50 }, L"[字][HV][字][HV]" },
    };
    for (const auto& testCase : testCases) {
        std::wstring decoded;
        if (!DecodeArib(testCase.data, decoded)) {
            diagnostic = tstring(_T("ARIB文字列を変換できません: ")) + testCase.name;
            return false;
        }
        if (decoded != testCase.expected) {
            diagnostic = strsprintf(_T("ARIB文字列が一致しません: %s: 期待=%s, 実際=%s"), testCase.name,
                wstring_to_tstring(testCase.expected).c_str(), wstring_to_tstring(decoded).c_str());
            return false;
        }
    }
    return true;
}

// テスト用の一時ファイル (デストラクタで削除する)
class TempFile {
public:
    explicit TempFile(const TCHAR* name) {
        path_ = std::filesystem::temp_directory_path() / (tstring(_T("amatsukaze_native_test_")) + name);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    tstring path() const { return path_.native(); }
    std::vector<uint8_t> read() const {
        std::ifstream file(path_, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
    void write(const std::vector<uint8_t>& data) const {
        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        file.write((const char*)data.data(), (std::streamsize)data.size());
    }
private:
    std::filesystem::path path_;
};

// LogoHeaderのコンストラクタはDLL外へ公開されないため、領域だけ確保して窓口で初期化する
struct LogoHeaderStorage {
    alignas(logo::LogoHeader) unsigned char bytes[sizeof(logo::LogoHeader)] = {};
    logo::LogoHeader* get() { return reinterpret_cast<logo::LogoHeader*>(bytes); }
};

// 少数・多数のマスク、暗部・明部、飽和、正負の相関を旧走査とbit単位で比較する。
// 8座標バッチの境界と端数、符号付きゼロも含める。
bool TestLogoCorrelationFusion(tstring& diagnostic) {
    constexpr int width = 48, height = 40, scaleCount = 32;
    std::vector<std::pair<int, int>> coordinates;
    for (int y = 2; y < height - 2; y++) {
        for (int x = 2; x < width - 2; x++) coordinates.emplace_back(x, y);
    }
    std::vector<float> work(width * height + 8);
    std::vector<float> kernels(coordinates.size() * 25 + 8);
    // idx==scaleCountの従来の境界参照を含め、末尾にも安全な番兵を置く。
    std::vector<LogoCorrelationScaleLimit> scales(coordinates.size() * scaleCount + 1);
    uint32_t state = 0x13579bdf;
    auto random = [&]() { state = state * 1664525u + 1013904223u; return state; };
    for (auto& k : kernels) k = ((int)(random() % 2001) - 1000) / 317.0f;
    for (auto& scale : scales) {
        scale.scale = ((int)(random() % 2001) - 1000) / 173.0f;
        scale.scale2 = (random() % 1001) / 1000.0f;
    }
    for (float maxv : { 255.0f, 1023.0f }) {
        for (int pattern = 0; pattern < 8; pattern++) {
            for (size_t i = 0; i < work.size(); i++) {
                switch (pattern) {
                case 0: work[i] = 0.0f; break;
                case 1: work[i] = maxv; break;
                case 2: work[i] = -maxv * 0.25f; break;
                case 3: work[i] = maxv * 1.1f; break;
                case 4: work[i] = (i & 1) ? maxv : 0.0f; break;
                case 5: work[i] = (random() % 1001) * (maxv / 1000.0f); break;
                case 6: work[i] = ((int)(random() % 1401) - 200) * (maxv / 1000.0f); break;
                default: work[i] = -0.0f; break;
                }
            }
            for (size_t count : { size_t(0), size_t(1), size_t(7), size_t(8), size_t(9), size_t(15),
                size_t(16), size_t(17), size_t(31), size_t(32), size_t(33), coordinates.size() }) {
                float reference = 0, fused = 0;
                if (!CompareLogoCorrelationForTest(kernels.data(), work.data(), width, coordinates.data(),
                    count, scales.data(), scaleCount, maxv, &reference, &fused)) return true;
                if (std::memcmp(&reference, &fused, sizeof(float)) != 0) {
                    diagnostic = strsprintf(_T("相関走査bit不一致: maxv=%.0f, pattern=%d, count=%d, 旧=%.9g, 新=%.9g"),
                        maxv, pattern, (int)count, reference, fused);
                    return false;
                }
            }
        }
    }
    return true;
}

// 共有ヘッダを使わずに記述した従来式と、画素固定パラメータをbit単位で照合する。
bool TestLogoResidualWeightParams(tstring& diagnostic) {
    const auto referenceTrust = [](double consistency) {
        return std::max(0.0, std::min(1.0, (consistency - 0.20) / 1.20));
    };
    const auto referenceSigma = [](double stdDiff, double trust) {
        const double sigmaBase = std::max(6.0 / 255.0, stdDiff);
        const double sigmaShrink = 1.0 + 1.65 * trust;
        return std::max(4.0 / 255.0, sigmaBase / sigmaShrink);
    };
    const auto sameBits = [](double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; };
    const auto verify = [&](double consistency, double stdDiff) {
        const double trust = referenceTrust(consistency);
        const bool enabled = trust > 1e-4;
        const auto prepared = logo::PrepareLogoResidualWeight(consistency, stdDiff);
        if (!sameBits(logo::CalcResidualReweightTrust(consistency), trust) || prepared.enabled != enabled ||
            !sameBits(prepared.sigma, enabled ? referenceSigma(stdDiff, trust) : 1.0)) {
            diagnostic = _T("残差重みの有効判定またはsigmaのbit不一致");
            return false;
        }
        return true;
    };
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto around = [&](double value) {
        return std::vector<double>({ std::nextafter(value, -inf), value, std::nextafter(value, inf) });
    };
    std::vector<double> consistencies = { -inf, -std::numeric_limits<double>::max(), -1.0, -0.0, 0.0,
        std::numeric_limits<double>::denorm_min(), 1.0, std::numeric_limits<double>::max(), inf, nan, -nan };
    for (double threshold : { 0.20, 0.20 + 1.20 * 1e-4, 1.40 }) {
        const auto values = around(threshold);
        consistencies.insert(consistencies.end(), values.begin(), values.end());
    }
    std::vector<double> stdDiffs = { -inf, -std::numeric_limits<double>::max(), -1.0, -0.0, 0.0,
        std::numeric_limits<double>::denorm_min(), 0.1, 1.0, std::numeric_limits<double>::max(), inf, nan, -nan };
    for (double threshold : { 4.0 / 255.0, 6.0 / 255.0, (4.0 / 255.0) * 2.65 }) {
        const auto values = around(threshold);
        stdDiffs.insert(stdDiffs.end(), values.begin(), values.end());
    }
    for (double consistency : consistencies) {
        for (double stdDiff : stdDiffs) if (!verify(consistency, stdDiff)) return false;
    }
    // NaN・無限大を含む信頼度を直接渡した場合も、従来のmaxの引数順を保つ。
    for (double trust : { -inf, -1.0, -0.0, 0.0, 1e-4, 1.0, inf, nan }) {
        for (double stdDiff : stdDiffs) {
            if (!Expect(sameBits(logo::CalcResidualReweightSigma(stdDiff, trust), referenceSigma(stdDiff, trust)),
                _T("残差重みsigmaの特殊値が従来式と一致しない"), diagnostic)) return false;
        }
    }
    if (!Expect(logo::PrepareLogoResidualWeight(nan, 0.1).enabled &&
        logo::PrepareLogoResidualWeight(inf, 0.1).enabled && !logo::PrepareLogoResidualWeight(-inf, 0.1).enabled,
        _T("NaNまたは無限大で従来のtrust閾値判定を維持できない"), diagnostic)) return false;
    uint64_t state = 0x6a09e667f3bcc909ULL;
    const auto randomBits = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (int i = 0; i < 20000; i++) {
        // 生の浮動小数点bit列と、実入力に近い有限値を交互に調べる。
        double consistency, stdDiff;
        const uint64_t consistencyBits = randomBits(), stdDiffBits = randomBits();
        if ((i & 1) == 0) {
            std::memcpy(&consistency, &consistencyBits, sizeof(double));
            std::memcpy(&stdDiff, &stdDiffBits, sizeof(double));
        } else {
            consistency = static_cast<double>(consistencyBits % 2000001) / 1000000.0 - 0.25;
            stdDiff = static_cast<double>(stdDiffBits % 1000001) / 1000000.0;
        }
        if (!verify(consistency, stdDiff)) return false;
    }
    return true;
}

// 省略条件をサンプルごとの従来式と照合し、非加重統計への復帰がbit一致することを確認する。
bool TestLogoResidualSkip(tstring& diagnostic) {
    for (bool valid : { false, true }) {
        for (bool gaussian : { false, true }) {
            for (bool lowBg : { false, true }) {
                logo::LogoResidualWeightParams params;
                params.enabled = gaussian;
                params.sigma = 0.05;
                const bool needed = logo::NeedsLogoResidualReweight(valid, params, lowBg);
                double rawF = 0.0, rawB = 0.0, weightedF = 0.0, weightedB = 0.0, sumWeight = 0.0;
                bool changedWeight = false;
                int count = 0;
                for (int fg = 0; fg < 256; fg += 7) {
                    for (int bg = 0; bg < 256; bg += 11) {
                        const double f = fg * (1.0f / 255.0f), b = bg * (1.0f / 255.0f);
                        double weight = 1.0;
                        if (valid) {
                            const double residual = b - (0.75 * f + 0.03);
                            if (gaussian) {
                                const double normalized = residual / params.sigma;
                                weight = std::max(1e-4, std::exp(-0.5 * normalized * normalized));
                            }
                            if (lowBg) {
                                const double gain = std::max(0.0, std::min(1.0, (0.25 - b) / 0.12));
                                const double side = std::max(0.0, std::min(1.0, (residual + 0.005) / 0.045));
                                weight = std::min(weight, std::max(0.05, 1.0 - 1.35 * gain * side));
                            }
                            weight = std::max(1e-4, weight);
                        }
                        changedWeight |= weight != 1.0;
                        if (!needed && weight != 1.0) {
                            diagnostic = _T("重みが変化するサンプルを再走査から除外した");
                            return false;
                        }
                        rawF += f; rawB += b;
                        weightedF += weight * f; weightedB += weight * b;
                        sumWeight += weight; count++;
                    }
                }
                if (needed && !changedWeight) {
                    diagnostic = _T("補正が必要な組合せを検査できていない");
                    return false;
                }
                if (!needed && (sumWeight != count || std::memcmp(&rawF, &weightedF, sizeof(double))
                    || std::memcmp(&rawB, &weightedB, sizeof(double)))) {
                    diagnostic = _T("省略画素の加重和と保持した非加重和がbit一致しない");
                    return false;
                }
            }
        }
    }
    return true;
}

// SIMD部分は従来のFMA、端数と境界は従来のスカラー演算を独立に再現する。
bool TestLogoBilateral(tstring& diagnostic) {
    if (!HasLogoBackgroundAVX2ForTest()) {
        _ftprintf(stdout, _T("AVX2非対応のためBilateral比較を省略\n"));
        return true;
    }
    std::mt19937 random(1025);
    for (int width : { 1, 2, 3, 4, 5, 11, 12, 13, 19, 20, 21, 27, 28, 29, 31, 32, 33, 84, 287, 288, 289 }) {
        for (int height : { 1, 2, 3, 5, 16, 37 }) {
            const int pitch = width + 7;
            std::vector<uint8_t> input(pitch * height + 1), actual(width * height + 32), expected(actual.size());
            float spatial[25], range[256];
            for (int k = 0; k < 25; k++) {
                spatial[k] = std::exp(-float((k / 5 - 2) * (k / 5 - 2) + (k % 5 - 2) * (k % 5 - 2)) / 8.0f);
            }
            for (int pattern = 0; pattern < 6; pattern++) {
                for (auto& pixel : input) pixel = pattern == 0 ? 0 : pattern == 1 ? 255 : random() & 255;
                const float sigma = 0.8f + pattern;
                for (int i = 0; i < 256; i++) range[i] = std::exp(-float(i * i) / (2.0f * sigma * sigma));
                for (int y0 = 0; y0 < height; y0 += std::max(1, height / 3)) {
                    const int y1 = std::min(height, y0 + std::max(1, height / 2));
                    std::fill(actual.begin(), actual.end(), 0xA5);
                    std::fill(expected.begin(), expected.end(), 0xA5);
                    if (!ApplyLogoBilateralAVX2ForTest(actual.data() + 16, input.data() + 1, pitch,
                        width, height, spatial, range, y0, y1)) {
                        diagnostic = _T("BilateralのSIMD実行に失敗した");
                        return false;
                    }
                    const int vectorEnd = 2 + std::max(0, width - 4) / 8 * 8;
                    for (int y = y0; y < y1; y++) {
                        for (int x = 0; x < width; x++) {
                            const int center = input[1 + y * pitch + x];
                            float sumW = 0.0f, sumV = 0.0f;
                            int k = 0;
                            const bool vectorPixel = x >= 2 && x < vectorEnd;
                            for (int dy = -2; dy <= 2; dy++) {
                                const int sy = (std::clamp)(y + dy, 0, height - 1);
                                for (int dx = -2; dx <= 2; dx++, k++) {
                                    const int sx = (std::clamp)(x + dx, 0, width - 1);
                                    const int value = input[1 + sy * pitch + sx];
                                    const float weight = spatial[k] * range[std::abs(value - center)];
                                    sumW += weight;
                                    if (vectorPixel) sumV = std::fma(weight, (float)value, sumV);
                                    else { const float product = weight * value; sumV += product; }
                                }
                            }
                            const float value = vectorPixel || sumW > 1e-8f ? sumV / sumW : (float)center;
                            expected[16 + y * width + x] = (uint8_t)(std::clamp)((int)(value + 0.5f), 0, 255);
                        }
                    }
                    if (actual != expected) {
                        diagnostic = strsprintf(_T("Bilateralの参照または番兵が不一致: 幅=%d 高さ=%d 模様=%d 行=%d..%d"), width, height, pattern, y0, y1);
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

// 行ピッチと番兵を含めて従来の整数式を独立に評価し、変換結果を全バイト比較する。
bool TestLogoRoiCacheCopy(tstring& diagnostic) {
    const auto verify = [&](auto pixel, int bitDepth, int width, int height, int padding, bool reverse = false) {
        using Pixel = decltype(pixel);
        const int rowPitch = width + padding;
        const int pitch = reverse ? -rowPitch : rowPitch;
        std::vector<Pixel> source((size_t)rowPitch * height + 3);
        for (size_t i = 0; i < source.size(); i++) source[i] = (Pixel)i;
        std::vector<uint8_t> expected((size_t)width * height + 32, 0xA5), actual(expected);
        const Pixel* src = source.data() + 1 + (reverse && height > 0 ? (size_t)(height - 1) * rowPitch : 0);
        const uint64_t maxv = std::max<uint64_t>(1, (uint64_t(1) << bitDepth) - 1);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const uint64_t value = src[(std::ptrdiff_t)y * pitch + x];
                const uint64_t scaled = (value * 255 + maxv / 2) / maxv;
                expected[16 + (size_t)y * width + x] = (uint8_t)std::min<uint64_t>(255, scaled);
            }
        }
        logo::CopyLogoRoiToCache(actual.data() + 16, src, pitch, width, height, bitDepth);
        if (actual != expected) {
            diagnostic = strsprintf(_T("ROI保存の変換または番兵が不一致: 型=%d bit=%d 幅=%d 高さ=%d 余白=%d"),
                (int)sizeof(Pixel), bitDepth, width, height, padding);
            return false;
        }
        return true;
    };
    for (int width : { 0, 1, 7, 16, 31, 32, 33, 84, 256, 288, 289 }) {
        for (int height : { 0, 1, 3, 17 }) {
            for (int padding : { 0, 3, 31 }) {
                for (int bitDepth : { 7, 8 }) {
                    if (!verify(uint8_t(0), bitDepth, width, height, padding)
                        || !verify(uint8_t(0), bitDepth, width, height, padding, true)) return false;
                }
                for (int bitDepth : { 8, 9, 10, 12, 16 }) {
                    if (!verify(uint16_t(0), bitDepth, width, height, padding)
                        || !verify(uint16_t(0), bitDepth, width, height, padding, true)) return false;
                }
            }
        }
    }
    // 8bit全256値と、16bit型の全65536値を各変換深度で検査する。
    if (!verify(uint8_t(0), 8, 256, 1, 0)) return false;
    for (int bitDepth : { 8, 9, 10, 12, 16 }) {
        if (!verify(uint16_t(0), bitDepth, 256, 256, 0)) return false;
    }
    return true;
}

bool TestLogoTemporalHistogram(tstring& diagnostic) {
    if (!Expect(logo::LogoTemporalHistogramIndex(1, 19, 0) == 1
        && logo::LogoTemporalHistogramIndex(0, 19, 1) == 16
        && logo::LogoTemporalHistogramIndex(15, 19, 255) == 4095
        && logo::LogoTemporalHistogramIndex(16, 19, 0) == 4096
        && logo::LogoTemporalHistogramIndex(16, 19, 1) == 4099
        && logo::LogoTemporalHistogramIndex(18, 19, 255) == 4863,
        _T("時間ヒストグラムの16laneと末尾3laneの配置が不正"), diagnostic)) return false;
    uint64_t state = 0xbb67ae8584caa73bULL;
    const auto randomBits = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (int width : { 0, 1, 16, 31, 32, 33, 37, 49, 65 }) {
        for (int height : { 0, 1, 31, 32, 33, 37, 49 }) {
            for (int margin : { 0, 1, 16 }) {
                const int innerWidth = std::max(0, width - margin * 2);
                const int innerHeight = std::max(0, height - margin * 2);
                const size_t pixels = static_cast<size_t>(innerWidth) * innerHeight;
                std::vector<uint16_t> reference(static_cast<size_t>(width) * height * 256, 0);
                std::vector<uint16_t> blocked(pixels * 256, 0);
                const auto visited = std::make_unique<uint8_t[]>(blocked.size());
                // 参照側は座標走査から連番を作り、共有ヘルパーの式を使わない。
                int nextInner = 0;
                for (int off = -1; off <= width * height; off++) {
                    const int x = width > 0 && off >= 0 ? off % width : -1;
                    const int y = width > 0 && off >= 0 ? off / width : -1;
                    const bool inside = x >= margin && x < width - margin
                        && y >= margin && y < height - margin;
                    const int expected = inside ? nextInner++ : -1;
                    const int actual = logo::LogoTemporalInnerPixelIndex(width, height, margin, off);
                    const int actualXY = logo::LogoTemporalInnerPixelIndex(width, height, margin, x, y);
                    if (!Expect(actual == expected && actualXY == expected,
                        _T("時間ヒストグラムの画素番号または座標からの内側連番が不一致"), diagnostic)) return false;
                    if (!inside) continue;
                    for (int bin = 0; bin < 256; bin++) {
                        const size_t index = logo::LogoTemporalHistogramIndex(actual, pixels, bin);
                        if (!Expect(index < blocked.size() && visited[index] == 0,
                            _T("時間ヒストグラムの添字が範囲外または重複"), diagnostic)) return false;
                        visited[index] = 1;
                        // 空の画素も残し、P5の未観測時の処理を検証する。
                        const uint16_t count = off % 5 == 0 ? 0 : static_cast<uint16_t>((off * 17 + bin * 13) % 9);
                        reference[static_cast<size_t>(off) * 256 + bin] = count;
                        blocked[index] = count;
                    }
                }
                if (!Expect(static_cast<size_t>(nextInner) == pixels
                    && std::all_of(visited.get(), visited.get() + blocked.size(), [](uint8_t value) { return value == 1; }),
                    _T("時間ヒストグラムの確保領域に未使用部分がある"), diagnostic)) return false;
                if (pixels == 0) continue;
                // 両レイアウトへ独立に飽和加算する。末尾ブロックも乱数更新の対象に含める。
                for (int sample = 0; sample < 2048; sample++) {
                    const int off = static_cast<int>(randomBits() % (static_cast<size_t>(width) * height));
                    const int bin = static_cast<int>(randomBits() % 256);
                    const int x = off % width, y = off / width;
                    if (x < margin || x >= width - margin || y < margin || y >= height - margin) continue;
                    auto& oldCount = reference[static_cast<size_t>(off) * 256 + bin];
                    if (oldCount < std::numeric_limits<uint16_t>::max()) oldCount++;
                    const int pixel = logo::LogoTemporalInnerPixelIndex(width, height, margin, off);
                    auto& newCount = blocked[logo::LogoTemporalHistogramIndex(pixel, pixels, bin)];
                    newCount = static_cast<uint16_t>(std::min<unsigned>(65535, unsigned(newCount) + 1));
                }
                // 内側25画素の末尾9laneで、65535を越える入力を与える。
                if (width == 37 && height == 37 && margin == 16) {
                    const int off = (height - margin - 1) * width + width - margin - 1;
                    const int pixel = logo::LogoTemporalInnerPixelIndex(width, height, margin, off);
                    for (int sample = 0; sample < 70000; sample++) {
                        auto& oldCount = reference[static_cast<size_t>(off) * 256 + 255];
                        if (oldCount < std::numeric_limits<uint16_t>::max()) oldCount++;
                        auto& newCount = blocked[logo::LogoTemporalHistogramIndex(pixel, pixels, 255)];
                        newCount = static_cast<uint16_t>(std::min<unsigned>(65535, unsigned(newCount) + 1));
                    }
                    if (!Expect(blocked[logo::LogoTemporalHistogramIndex(pixel, pixels, 255)] == 65535,
                        _T("時間ヒストグラムのuint16飽和加算が不正"), diagnostic)) return false;
                }
                for (int y = margin; y < height - margin; y++) {
                    for (int x = margin; x < width - margin; x++) {
                        const int off = y * width + x;
                        const int pixel = logo::LogoTemporalInnerPixelIndex(width, height, margin, off);
                        int oldTotal = 0, newTotal = 0;
                        for (int bin = 0; bin < 256; bin++) {
                            const uint16_t oldCount = reference[static_cast<size_t>(off) * 256 + bin];
                            const uint16_t newCount = blocked[logo::LogoTemporalHistogramIndex(pixel, pixels, bin)];
                            if (!Expect(oldCount == newCount, _T("時間ヒストグラムの全bin比較が不一致"), diagnostic)) return false;
                            oldTotal += oldCount;
                            newTotal += newCount;
                        }
                        // 旧P5は累積加算、新P5は残り順位を減算し、演算実装を共有しない。
                        float oldP5 = 0.0f, newP5 = 0.0f;
                        if (oldTotal > 0) {
                            const int target = std::max(1, static_cast<int>(std::ceil(oldTotal * 0.05f)));
                            int accum = 0;
                            for (int bin = 0; bin < 256; bin++) {
                                accum += reference[static_cast<size_t>(off) * 256 + bin];
                                if (accum >= target) { oldP5 = bin / 255.0f; break; }
                            }
                        }
                        if (newTotal > 0) {
                            int remaining = std::max(1, static_cast<int>(std::ceil(newTotal * 0.05f)));
                            for (int bin = 0; bin < 256; bin++) {
                                remaining -= blocked[logo::LogoTemporalHistogramIndex(pixel, pixels, bin)];
                                if (remaining <= 0) { newP5 = static_cast<float>(bin) / 255; break; }
                            }
                        }
                        if (!Expect(std::memcmp(&oldP5, &newP5, sizeof(float)) == 0,
                            _T("時間ヒストグラムのP5がbit不一致"), diagnostic)) return false;
                    }
                }
            }
        }
    }
    if (!Expect(logo::LogoTemporalInnerPixelIndex(37, 37, -1, 0) == -1
        && logo::LogoTemporalInnerPixelIndex(37, 37, -1, 0, 0) == -1
        && logo::LogoTemporalInnerPixelIndex(37, 37, 0, 37, 0) == -1
        && logo::LogoTemporalInnerPixelIndex(37, 37, 0, 0, 37) == -1
        && logo::LogoTemporalInnerPixelIndex(37, 37, 0, 0, -1) == -1,
        _T("時間ヒストグラムの不正な余白または領域外の座標を受理した"), diagnostic)) return false;
    return true;
}

bool TestLogoCollectPartition(tstring& diagnostic) {
    if (!Expect(logo::BuildLogoCollectXBoundaries(288, 16, 32, 2, true) == std::vector<int>({ 16, 128, 272 }),
        _T("実素材相当の32画素境界が一致しない"), diagnostic)) return false;
    if (!Expect(logo::BuildLogoCollectXBoundaries(37, 16, 32, 5, true) == std::vector<int>({ 16, 17, 18, 19, 20, 21 }),
        _T("狭い区間の非整列境界を維持できない"), diagnostic)) return false;
    if (!Expect(logo::BuildLogoCollectXBoundaries(16, 16, 32, 1, true) == std::vector<int>({ 16, 16 }),
        _T("空のROI境界が一致しない"), diagnostic)) return false;
    // 乗算はintの範囲を超えても、返す境界は元の幅に収まる。
    const auto large = logo::BuildLogoCollectXBoundaries(std::numeric_limits<int>::max(), 16, 32, 3, false);
    if (!Expect(large[2] == 16 + static_cast<int>((int64_t(std::numeric_limits<int>::max()) - 32) * 2 / 3),
        _T("大きい幅の分割で整数オーバーフロー"), diagnostic)) return false;
    for (int width : { 0, 1, 16, 31, 32, 33, 37, 63, 64, 65, 95, 96, 127, 128, 129, 288, 480, 1024 }) {
        for (int margin : { 0, 1, 16, 32 }) {
            const int innerWidth = std::max(0, width - margin * 2);
            const int maxSplits = std::max(1, innerWidth);
            for (int splits = 1; splits <= maxSplits; splits++) {
                for (int radius : { 0, 1, 4, 16, 32, 48, 65, 512 }) {
                    for (bool aligned : { false, true }) {
                        const auto boundaries = logo::BuildLogoCollectXBoundaries(width, margin, radius, splits, aligned);
                        if (boundaries.size() != static_cast<size_t>(splits) + 1 || boundaries.front() != margin ||
                            boundaries.back() != margin + innerWidth) {
                            diagnostic = _T("分割の個数または外端を維持できない");
                            return false;
                        }
                        std::vector<int> coverage(innerWidth, 0);
                        for (int tile = 0; tile < splits; tile++) {
                            if (boundaries[tile] > boundaries[tile + 1] ||
                                (innerWidth > 0 && boundaries[tile] == boundaries[tile + 1])) {
                                diagnostic = _T("分割境界の単調性を維持できない");
                                return false;
                            }
                            for (int x = boundaries[tile]; x < boundaries[tile + 1]; x++) {
                                if (x < margin || x >= margin + innerWidth) {
                                    diagnostic = _T("収集対象外の画素を分割に含めた");
                                    return false;
                                }
                                coverage[x - margin]++;
                            }
                        }
                        if (!std::all_of(coverage.begin(), coverage.end(), [](int count) { return count == 1; })) {
                            diagnostic = _T("分割による画素の重複または欠落");
                            return false;
                        }
                        for (int tile = 1; tile < splits; tile++) {
                            const int original = margin + innerWidth * tile / splits;
                            if ((!aligned && boundaries[tile] != original) ||
                                (aligned && boundaries[tile] != original && (boundaries[tile] - std::max(margin, radius)) % 32 != 0)) {
                                diagnostic = _T("非整列時の互換性または32画素整列が不正");
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}

// 救済辺の選択順、上下左右境界、閾値ちょうどの判定をスカラー処理とbit比較する。
bool TestLogoBackgroundRetry(tstring& diagnostic) {
    if (!HasLogoBackgroundAVX2ForTest()) {
        _ftprintf(stdout, _T("AVX2非対応のため背景推定SIMD比較を省略\n"));
        return true;
    }
    // 救済での最初の一致、全有効辺の平均、画像外の辺を飛ばす場合を固定値で確認する。
    for (int scenario = 0; scenario < 3; scenario++) {
        constexpr int width = 128, height = 96, radius = 4, y = 48;
        const int x = scenario == 2 ? radius : 32;
        std::vector<uint8_t> frame(width * height);
        for (int row = 0; row < height; row++) {
            for (int col = 0; col < width; col++) frame[col + row * width] = ((row + col) & 1) ? 0 : 255;
        }
        for (int col = 0; col < width; col++) {
            frame[col + (y - radius) * width] = 100;
            frame[col + (y + radius) * width] = 200;
            frame[col + (y + radius + 16) * width] = 200;
        }
        for (int row = y - radius; row <= y + radius; row++) {
            if (x - radius - 16 >= 0) frame[x - radius - 16 + row * width] = scenario == 0 ? 100 : 140;
            frame[x + radius + 16 + row * width] = scenario == 2 ? 120 : 160;
        }
        float reference[32] = {}, vectorized[32] = {};
        uint32_t referenceMask = 0, vectorizedMask = 0;
        if (!Expect(CompareLogoBackgroundForTest(frame.data(), width, height, x, y, radius, 0,
            reference, vectorized, &referenceMask, &vectorizedMask) != 0,
            _T("救済固定値比較の実行失敗"), diagnostic)) return false;
        const float expected = scenario == 0 ? 100.0f : scenario == 1 ? 150.0f : 140.0f;
        if (!Expect((referenceMask & 1) && (vectorizedMask & 1) && reference[0] == expected && vectorized[0] == expected,
            _T("救済辺の選択順または全有効辺平均の不一致"), diagnostic)) return false;
    }
    uint32_t randomState = 0x625a9b17;
    auto random = [&]() { randomState = randomState * 1664525u + 1013904223u; return randomState; };
    for (int radius : { 1, 4, 8, 16, 32 }) {
        for (int extraWidth : { 0, 37 }) {
            const int width = radius * 2 + 32 + extraWidth;
            constexpr int height = 161;
            std::vector<uint8_t> frame(width * height);
            for (int pattern = 0; pattern < 8; pattern++) {
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        int value = 0;
                        switch (pattern) {
                        case 0: value = 100; break;
                        case 1: value = (x * 7 + y * 11) & 255; break;
                        case 2: value = random() >> 24; break;
                        case 3: value = 100 + random() % 13; break;
                        case 4: value = ((x / 13 + y / 17) & 1) ? 100 : 200; break;
                        case 5: value = y % 19 == 0 ? 100 : ((x + y) & 1) * 255; break;
                        case 6: value = x % 23 == 0 ? 100 : ((x + y) & 1) * 255; break;
                        default: value = ((x / 11 + y / 7) % 3) * 100; break;
                        }
                        frame[x + y * width] = (uint8_t)value;
                    }
                }
                for (int x : { radius, width - radius - 32 }) {
                    for (int y : { 0, radius - 1, radius, radius + 15, radius + 16,
                        height / 2, height - radius - 17, height - radius - 1, height - 1 }) {
                        for (int threshold : { 0, 1, 11, 12, 13, 32, 255 }) {
                            float reference[32] = {}, vectorized[32] = {};
                            uint32_t referenceMask = 0, vectorizedMask = 0;
                            if (!Expect(CompareLogoBackgroundForTest(frame.data(), width, height, x, y, radius,
                                threshold, reference, vectorized, &referenceMask, &vectorizedMask) != 0,
                                _T("背景推定比較の実行失敗"), diagnostic)) return false;
                            if (referenceMask != vectorizedMask) {
                                diagnostic = strsprintf(_T("背景推定マスク不一致: 半径=%d, 幅=%d, 模様=%d, x=%d, y=%d, 閾値=%d"),
                                    radius, width, pattern, x, y, threshold);
                                return false;
                            }
                            for (int lane = 0; lane < 32; lane++) {
                                if ((referenceMask & (1u << lane)) &&
                                    std::memcmp(reference + lane, vectorized + lane, sizeof(float)) != 0) {
                                    diagnostic = strsprintf(_T("背景推定値bit不一致: 半径=%d, 幅=%d, 模様=%d, x=%d, y=%d, 閾値=%d, lane=%d, 旧=%.9g, 新=%.9g"),
                                        radius, width, pattern, x, y, threshold, lane, reference[lane], vectorized[lane]);
                                    return false;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool TestLogoFile(tstring& diagnostic) {
    constexpr int LOGO_W = 8, LOGO_H = 4, LOG_UV = 1;
    constexpr size_t SIZE_Y = LOGO_W * LOGO_H;
    constexpr size_t SIZE_UV = (LOGO_W >> LOG_UV) * (LOGO_H >> LOG_UV);
    constexpr size_t PLANE_VALUES = (SIZE_Y + SIZE_UV * 2) * 2;
    LogoHeaderStorage headerStorage;
    // AviUtl互換部分(31文字まで)より長い名前も拡張ヘッダには残る
    constexpr const char* LOGO_NAME = "AMT TEST LOGO 1024 with a name longer than thirty-one characters";
    InitLogoHeaderForTest(headerStorage.get(), LOGO_W, LOGO_H, LOG_UV, LOG_UV, 1920, 1080, 1500, 50, LOGO_NAME);
    logo::LogoHeader& header = *headerStorage.get();
    header.serviceId = 1024;
    // aY, bY, aU, bU, aV, bV。係数は「元の画素 = A * 観測値 + B」の形で、A = 1 / (1 - 不透明度)。
    // 不透明度20%前後の白いロゴ (A≒1.25, B≒-0.25) とし、先頭画素(とそれを含むUV画素)は完全透明(A=1, B=0)にする
    std::vector<float> planes(PLANE_VALUES);
    const size_t planeOffsets[] = { 0, SIZE_Y, SIZE_Y * 2, SIZE_Y * 2 + SIZE_UV, SIZE_Y * 2 + SIZE_UV * 2, SIZE_Y * 2 + SIZE_UV * 3, PLANE_VALUES };
    for (int plane = 0; plane < 6; plane++) {
        const bool isA = (plane % 2) == 0;
        for (size_t i = planeOffsets[plane]; i < planeOffsets[plane + 1]; i++) {
            planes[i] = isA ? 1.25f + 0.001f * (float)(i - planeOffsets[plane]) : -0.25f - 0.001f * (float)(i - planeOffsets[plane]);
        }
    }
    planes[0] = 1.0f;                           // aY[0]
    planes[SIZE_Y] = 0.0f;                      // bY[0]
    planes[SIZE_Y * 2] = 1.0f;                  // aU[0]
    planes[SIZE_Y * 2 + SIZE_UV] = 0.0f;        // bU[0]
    planes[SIZE_Y * 2 + SIZE_UV * 2] = 1.0f;    // aV[0]
    planes[SIZE_Y * 2 + SIZE_UV * 3] = 0.0f;    // bV[0]

    TempFile logoFile(_T("logo.lgd"));
    if (!Expect(SaveLogoForTest(logoFile.path().c_str(), &header, planes.data(), planes.size(), 0) == 0,
        _T("ロゴを保存できません"), diagnostic)) return false;

    // 保存→読み込みでヘッダと係数が完全に一致する
    LogoHeaderStorage loadedStorage;
    logo::LogoHeader& loadedHeader = *loadedStorage.get();
    std::vector<float> loadedPlanes(PLANE_VALUES, -1.0f);
    size_t loadedCount = 0;
    if (!Expect(LoadLogoForTest(logoFile.path().c_str(), &loadedHeader, loadedPlanes.data(), loadedPlanes.size(), &loadedCount) == 0
        && loadedCount == PLANE_VALUES, _T("保存したロゴを読み込めません"), diagnostic)) return false;
    if (!Expect(std::memcmp(&loadedHeader, &header, sizeof(header)) == 0 && std::strcmp(loadedHeader.name, LOGO_NAME) == 0
        && loadedHeader.magic == 0x12345 && loadedHeader.version == 1, _T("読み込んだロゴヘッダが一致しません"), diagnostic)) return false;
    if (!Expect(std::memcmp(loadedPlanes.data(), planes.data(), planes.size() * sizeof(float)) == 0,
        _T("読み込んだロゴ係数が一致しません"), diagnostic)) return false;

    // 先頭はAviUtl互換部分: ファイルヘッダ、LOGO_HEADER、LOGO_PIXEL、その後に拡張部分が続く
    const auto bytes = logoFile.read();
    const size_t baseSize = sizeof(LOGO_FILE_HEADER) + sizeof(LOGO_HEADER) + SIZE_Y * sizeof(LOGO_PIXEL);
    if (!Expect(bytes.size() == baseSize + sizeof(logo::LogoHeader) + PLANE_VALUES * sizeof(float),
        _T("ロゴファイルのサイズが一致しません"), diagnostic)) return false;
    LOGO_FILE_HEADER fileHeader;
    LOGO_HEADER logoHeader;
    std::memcpy(&fileHeader, bytes.data(), sizeof(fileHeader));
    std::memcpy(&logoHeader, bytes.data() + sizeof(fileHeader), sizeof(logoHeader));
    if (!Expect(std::memcmp(fileHeader.str, LOGO_FILE_HEADER_STR, LOGO_FILE_HEADER_STR_SIZE) == 0
        && fileHeader.logonum.c[0] == 0 && fileHeader.logonum.c[3] == 1, _T("AviUtlロゴファイルヘッダが一致しません"), diagnostic)) return false;
    if (!Expect(std::strncmp(logoHeader.name, LOGO_NAME, LOGO_MAX_NAME - 1) == 0 && logoHeader.name[LOGO_MAX_NAME - 1] == '\0'
        && logoHeader.x == 1500 && logoHeader.y == 50
        && logoHeader.w == LOGO_W && logoHeader.h == LOGO_H, _T("AviUtlロゴヘッダが一致しません"), diagnostic)) return false;
    std::vector<LOGO_PIXEL> pixels(SIZE_Y);
    std::memcpy(pixels.data(), bytes.data() + sizeof(fileHeader) + sizeof(logoHeader), SIZE_Y * sizeof(LOGO_PIXEL));
    // YC48への変換で量子化されるため、完全透明(A=1)でも不透明度はわずかに残る (1%未満)
    constexpr short NEARLY_TRANSPARENT_DP = LOGO_MAX_DP / 100;
    if (pixels[0].y != 0 || pixels[0].cb != 0 || pixels[0].cr != 0 || pixels[0].dp_y >= NEARLY_TRANSPARENT_DP
        || pixels[0].dp_cb >= NEARLY_TRANSPARENT_DP || pixels[0].dp_cr >= NEARLY_TRANSPARENT_DP) {
        diagnostic = strsprintf(_T("完全透明の画素がほぼ透明になりません: dp_y=%d y=%d dp_cb=%d cb=%d dp_cr=%d cr=%d"),
            pixels[0].dp_y, pixels[0].y, pixels[0].dp_cb, pixels[0].cb, pixels[0].dp_cr, pixels[0].cr);
        return false;
    }
    if (!(pixels[2].dp_y > 0 && pixels[2].dp_y < LOGO_MAX_DP && pixels[2].dp_cb > 0 && pixels[2].dp_cr > 0)) {
        diagnostic = strsprintf(_T("半透明の画素の不透明度が範囲外です: dp_y=%d y=%d dp_cb=%d cb=%d dp_cr=%d cr=%d"),
            pixels[2].dp_y, pixels[2].y, pixels[2].dp_cb, pixels[2].cb, pixels[2].dp_cr, pixels[2].cr);
        return false;
    }

    // AviUtl互換部分だけのファイルは、拡張部分がないので読み込みに失敗する (異常終了しない)
    TempFile aviUtlFile(_T("aviutl.lgd"));
    if (!Expect(SaveLogoForTest(aviUtlFile.path().c_str(), &header, planes.data(), planes.size(), 1) == 0
        && aviUtlFile.read().size() == baseSize, _T("AviUtl形式のロゴを保存できません"), diagnostic)) return false;
    if (!Expect(LoadLogoForTest(aviUtlFile.path().c_str(), &loadedHeader, loadedPlanes.data(), loadedPlanes.size(), &loadedCount) != 0,
        _T("拡張部分のないロゴを読み込めてしまいました"), diagnostic)) return false;

    // 拡張ヘッダのmagicが壊れたファイルは読み込まない
    auto broken = bytes;
    broken[baseSize] ^= 0xFF;
    TempFile brokenFile(_T("broken.lgd"));
    brokenFile.write(broken);
    if (!Expect(LoadLogoForTest(brokenFile.path().c_str(), &loadedHeader, loadedPlanes.data(), loadedPlanes.size(), &loadedCount) != 0,
        _T("magicが不正なロゴを読み込めてしまいました"), diagnostic)) return false;

    // 係数が途中で切れたファイルは読み込まない
    TempFile truncatedFile(_T("truncated.lgd"));
    truncatedFile.write(std::vector<uint8_t>(bytes.begin(), bytes.end() - sizeof(float)));
    return Expect(LoadLogoForTest(truncatedFile.path().c_str(), &loadedHeader, loadedPlanes.data(), loadedPlanes.size(), &loadedCount) != 0,
        _T("途中で切れたロゴを読み込めてしまいました"), diagnostic);
}

bool RunCaptionStreamCase(int testCase, tstring& diagnostic) {
    char message[2048] = {};
    if (CheckCaptionStreamForTest(testCase, message, sizeof(message)) == 1) return true;
    // DLL 側の診断メッセージは UTF-8
    diagnostic = char_to_tstring(message, CP_UTF8);
    return false;
}

bool TestCaptionPesSerialization(tstring& diagnostic) { return RunCaptionStreamCase(0, diagnostic); }
bool TestCaptionPesWrap(tstring& diagnostic) { return RunCaptionStreamCase(1, diagnostic); }
bool TestCaptionIntervalMapping(tstring& diagnostic) { return RunCaptionStreamCase(2, diagnostic); }

bool TestCaptionPgsCanvasSize(tstring& diagnostic) {
    // PGSのキャンバスは出力映像の表示サイズ (SAR反映後) で、長辺4096を超えると比例縮小する
    const struct {
        const TCHAR* name;
        int width, height, sarWidth, sarHeight, userSarWidth, userSarHeight;
        int expectedWidth, expectedHeight;
    } testCases[] = {
        { _T("1440x1080 SAR4:3"), 1440, 1080, 4, 3, 0, 0, 1920, 1080 },
        { _T("1920x1080 SAR1:1"), 1920, 1080, 1, 1, 0, 0, 1920, 1080 },
        { _T("720x480 SAR32:27 (SD 16:9)"), 720, 480, 32, 27, 0, 0, 853, 480 },
        // SARが1未満なら幅を縮めず高さを伸ばす
        { _T("720x480 SAR8:9 (SD 4:3)"), 720, 480, 8, 9, 0, 0, 720, 540 },
        { _T("SAR不明"), 1440, 1080, 0, 0, 0, 0, 1440, 1080 },
        { _T("SARの片方だけ不明"), 1440, 1080, 4, 0, 0, 0, 1440, 1080 },
        { _T("ユーザーSAR優先"), 1440, 1080, 4, 3, 1, 1, 1440, 1080 },
        { _T("ユーザーSARの片方だけ指定は無視"), 1440, 1080, 4, 3, 1, 0, 1920, 1080 },
        { _T("長辺4096ちょうど"), 4096, 2160, 1, 1, 0, 0, 4096, 2160 },
        { _T("SAR反映で4096超え"), 3840, 2160, 4, 3, 0, 0, 4096, 1728 },
        { _T("縦長で4096超え"), 1080, 4000, 3, 4, 0, 0, 829, 4096 },
        { _T("8K"), 7680, 4320, 1, 1, 0, 0, 4096, 2304 },
    };
    for (const auto& testCase : testCases) {
        int canvasWidth = 0, canvasHeight = 0;
        if (CaptionPgsCanvasSizeForTest(testCase.width, testCase.height, testCase.sarWidth, testCase.sarHeight,
                testCase.userSarWidth, testCase.userSarHeight, &canvasWidth, &canvasHeight) != 1) {
            diagnostic = strsprintf(_T("%s: キャンバスサイズを計算できません"), testCase.name);
            return false;
        }
        if (canvasWidth != testCase.expectedWidth || canvasHeight != testCase.expectedHeight) {
            diagnostic = strsprintf(_T("%s: 期待値=%dx%d, 実際=%dx%d"), testCase.name,
                testCase.expectedWidth, testCase.expectedHeight, canvasWidth, canvasHeight);
            return false;
        }
    }
    int canvasWidth = 0, canvasHeight = 0;
    if (!Expect(CaptionPgsCanvasSizeForTest(0, 1080, 1, 1, 0, 0, &canvasWidth, &canvasHeight) == 0,
        _T("幅0を受け付けてしまいました"), diagnostic)) return false;
    return Expect(CaptionPgsCanvasSizeForTest(1920, 1080, 1, 1, 0, 0, nullptr, &canvasHeight) == 0,
        _T("出力先nullを受け付けてしまいました"), diagnostic);
}


struct TestCase {
    const TCHAR* name;
    bool (*run)(tstring& diagnostic);
};

constexpr TestCase TEST_CASES[] = {
    { _T("caption_text_length"), TestCaptionTextLength },
    { _T("bitrate_zones"), TestBitrateZones },
    { _T("vfr_input_detection"), TestVFRInputDetection },
    { _T("crc32"), TestCRC32 },
    { _T("bit_reader"), TestBitReader },
    { _T("auto_buffer"), TestAutoBuffer },
    { _T("encoder_option"), TestEncoderOption },
    { _T("mpeg2_video_parser"), TestMpeg2VideoParser },
    { _T("h264_video_parser"), TestH264VideoParser },
    { _T("h264_pan_scan"), TestH264PanScan },
    { _T("adts_parser"), TestAdtsParser },
    { _T("audio_pes_pipeline"), TestAudioPesPipeline },
    { _T("audio_track_separate_planner"), TestAudioTrackSeparatePlanner },
    { _T("audio_track_merge_planner"), TestAudioTrackMergePlanner },
    { _T("audio_silent_adts_bytes"), TestAudioSilentAdtsBytes },
    { _T("audio_channel_conversion"), TestAudioChannelConversion },
    { _T("command_line_arguments"), TestCommandLineArguments },
    { _T("cm_cut_offset"), TestCMCutOffset },
    { _T("cm_cut_offset_command_line"), TestCMCutOffsetCommandLine },
    { _T("wave_header"), TestWaveHeader },
    { _T("audio_silent_adts_decode"), TestAudioSilentAdtsDecode },
    { _T("dual_mono_splitter"), TestDualMonoSplitter },
    { _T("bitrate_zones_properties"), TestBitrateZonesProperties },
    { _T("ps_writer_round_trip"), TestPsWriterRoundTrip },
    { _T("audio_aac_decoder_agreement"), TestAudioAacDecoderAgreement },
    { _T("audio_convert_position"), TestAudioConvertPosition },
    { _T("audio_convert_dual_mono"), TestAudioConvertDualMono },
    { _T("audio_convert_long_gap"), TestAudioConvertLongGap },
    { _T("audio_reform_separate"), TestAudioReformSeparate },
    { _T("audio_reform_merge"), TestAudioReformMerge },
    { _T("audio_reform_unsupported_fallback"), TestAudioReformUnsupportedFallback },
    { _T("audio_reform_cfg0_dual_mono"), TestAudioReformCfg0DualMono },
    { _T("audio_builder_copy_silence"), TestAudioBuilderCopySilence },
    { _T("audio_builder_dual_mono"), TestAudioBuilderDualMono },
    { _T("audio_builder_reject_pce"), TestAudioBuilderRejectPce },
    { _T("audio_builder_merge_convert"), TestAudioBuilderMergeConvert },
    { _T("ts_selector"), TestTsSelector },
    { _T("arib_string"), TestAribString },
    { _T("logo_file"), TestLogoFile },
    { _T("logo_correlation_fusion"), TestLogoCorrelationFusion },
    { _T("logo_background_retry"), TestLogoBackgroundRetry },
    { _T("logo_collect_partition"), TestLogoCollectPartition },
    { _T("logo_residual_weight_params"), TestLogoResidualWeightParams },
    { _T("logo_residual_skip"), TestLogoResidualSkip },
    { _T("logo_bilateral"), TestLogoBilateral },
    { _T("logo_roi_cache_copy"), TestLogoRoiCacheCopy },
    { _T("logo_temporal_histogram"), TestLogoTemporalHistogram },
    { _T("caption_pes_serialization"), TestCaptionPesSerialization },
    { _T("caption_pes_wrap"), TestCaptionPesWrap },
    { _T("caption_interval_mapping"), TestCaptionIntervalMapping },
    { _T("caption_pgs_canvas_size"), TestCaptionPgsCanvasSize },
};

void PrintUsage(const TCHAR* program) {
    _ftprintf(stdout, _T("使用法: %s [--list] [--select <ケース名>]...\n"), program);
}

bool IsSelected(const TestCase& testCase, const std::vector<tstring>& selectedNames) {
    return selectedNames.empty()
        || std::find(selectedNames.begin(), selectedNames.end(), testCase.name) != selectedNames.end();
}

bool IsRegisteredName(const tstring& name) {
    return std::any_of(std::begin(TEST_CASES), std::end(TEST_CASES), [&name](const TestCase& testCase) {
        return name == testCase.name;
    });
}

int RunTestCase(const TestCase& testCase) {
    try {
        tstring diagnostic;
        if (testCase.run(diagnostic)) {
            _ftprintf(stdout, _T("[PASS] %s\n"), testCase.name);
            return 0;
        }
        _ftprintf(stderr, _T("[FAIL] %s: %s\n"), testCase.name, diagnostic.c_str());
    } catch (const Exception& exception) {
        _ftprintf(stderr, _T("[FAIL] %s: Amatsukaze例外: %s\n"), testCase.name, exception.message());
    } catch (const std::exception& exception) {
        _ftprintf(stderr, _T("[FAIL] %s: 例外: %s\n"), testCase.name, char_to_tstring(exception.what()).c_str());
    } catch (...) {
        _ftprintf(stderr, _T("[FAIL] %s: 不明な例外です。\n"), testCase.name);
    }
    return 1;
}

}

int _tmain(int argc, TCHAR* argv[]) {
    // ワイド文字の出力で日本語を変換できるよう、文字種別だけ環境のロケールに合わせる
    // (数値の書式はstd::stod等に影響するため変更しない)
    std::setlocale(LC_CTYPE, "");
    bool listOnly = false;
    std::vector<tstring> selectedNames;
    for (int i = 1; i < argc; i++) {
        const tstring argument = argv[i];
        if (argument == _T("--list")) {
            listOnly = true;
        } else if (argument == _T("--select")) {
            if (++i >= argc) {
                _ftprintf(stderr, _T("--select にはケース名が必要です。\n"));
                PrintUsage(argv[0]);
                return 2;
            }
            selectedNames.emplace_back(argv[i]);
        } else if (argument == _T("--help") || argument == _T("-h")) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            _ftprintf(stderr, _T("不明な引数です: %s\n"), argument.c_str());
            PrintUsage(argv[0]);
            return 2;
        }
    }
    for (const auto& name : selectedNames) {
        if (!IsRegisteredName(name)) {
            _ftprintf(stderr, _T("未登録のケースです: %s。--list で確認してください。\n"), name.c_str());
            return 2;
        }
    }
    if (listOnly) {
        for (const auto& testCase : TEST_CASES) _ftprintf(stdout, _T("%s\n"), testCase.name);
        return 0;
    }

    int selectedCount = 0;
    int failureCount = 0;
    for (const auto& testCase : TEST_CASES) {
        if (!IsSelected(testCase, selectedNames)) continue;
        selectedCount++;
        failureCount += RunTestCase(testCase);
    }
    if (selectedCount == 0) {
        _ftprintf(stderr, _T("選択したケースは登録されていません。--list で確認してください。\n"));
        return 2;
    }
    _ftprintf(stdout, _T("実行: %d件, 失敗: %d件\n"), selectedCount, failureCount);
    return failureCount == 0 ? 0 : 1;
}
