// Amatsukaze の公開ネイティブ単体テスト実行器
#include "CaptionData.h"
#include "EncoderOptionParser.h"
#include "FilteredSource.h"
#include "StreamReform.h"
#include "StreamUtils.h"
#include "TsSplitter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <clocale>
#include <cstring>
#include <exception>
#include <iterator>
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
    for (int i = 0; i < normalCount; ++i) intervals.push_back({ normal, normalRepeat });
    for (int i = 0; i < alternateCount; ++i) intervals.push_back({ alternate, alternateRepeat });
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
    for (uint32_t v = value; v != 0; v >>= 1) ++bitLength;
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
        for (int i = 0; i < VALUE_COUNT; ++i) {
            seed = seed * 1103515245u + 12345u;
            values.push_back((seed >> 8) & ((1u << (i % 16)) - 1));
            writer.writen(i & 1, 1 + (i % 7)); // ずれを作るための固定長フィールド
            WriteExpGolomb(writer, values.back());
        }
        writer.byteAlign<true>();
        writer.flush();
        BitReader reader(buffer.get());
        for (int i = 0; i < VALUE_COUNT; ++i) {
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
    for (int i = 0; i < TOTAL_BYTES; ++i) {
        buffer.add(uint8_t(nextValue++ & 0xFF));
        if ((int)buffer.size() > KEEP_BYTES) {
            const int trim = (int)buffer.size() - KEEP_BYTES + (i % 5);
            buffer.trimHead(trim);
            headValue += std::min(trim, nextValue - headValue);
        }
    }
    if (!Expect((int)buffer.size() == nextValue - headValue, _T("繰り返し追加後のサイズが一致しません"), diagnostic)) return false;
    for (size_t i = 0; i < buffer.size(); ++i) {
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
        for (int i = 0; matched && i < 3; ++i) matched = std::abs(info.rcModeValue[i] - testCase.expectedValues[i]) < 1e-9;
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
        for (uint32_t v = value; v != 0; v >>= 1) ++bitLength;
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
    for (size_t i = 0; i < units.size(); ++i) {
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
    for (size_t i = 0; i < units.size(); ++i) timestamps.emplace_back(1000 + 3003 * (int64_t)i, 900 + 3003 * (int64_t)i);
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
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!ExpectVideoFrame(frames[i], expectedFrames[i], i, diagnostic)) return false;
    }
    // SAR = DAR(16:9) * 1080 / 1440 = 4:3、色情報なしは2(未指定)
    if (!ExpectVideoFormat(frames[0].format, { 1440, 1080, 1440, 1080, 4, 3, 30000, 1001, 2, false }, diagnostic)) return false;
    if (!Expect(frames[0].format.format == VS_MPEG2 && frames[0].format.mpeg2BitRateValue == 50000
        && frames[0].format.mpeg2VbvBufferSizeValue == 488 && frames[0].format.mpeg2ProfileAndLevelIndication == 0x44,
        _T("MPEG-2のビットレート/VBV/プロファイルが一致しません"), diagnostic)) return false;
    if (!Expect(!frames[0].progressive, _T("インタレースのフレームがprogressive扱いです"), diagnostic)) return false;

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
    for (size_t i = 0; i < frames.size(); ++i) {
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
    for (int i = 0; i < sliceCount; ++i) AppendSlice(unit, withParameterSets ? H264_NAL_IDR_SLICE : H264_NAL_SLICE);
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
    for (size_t i = 0; i < units.size(); ++i) timestamps.emplace_back(10003 + 3003 * (int64_t)i, 7000 + 3003 * (int64_t)i);
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
    for (size_t i = 0; i < frames.size(); ++i) {
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
    { _T("caption_pes_serialization"), TestCaptionPesSerialization },
    { _T("caption_pes_wrap"), TestCaptionPesWrap },
    { _T("caption_interval_mapping"), TestCaptionIntervalMapping },
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
    for (int i = 1; i < argc; ++i) {
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
        ++selectedCount;
        failureCount += RunTestCase(testCase);
    }
    if (selectedCount == 0) {
        _ftprintf(stderr, _T("選択したケースは登録されていません。--list で確認してください。\n"));
        return 2;
    }
    _ftprintf(stdout, _T("実行: %d件, 失敗: %d件\n"), selectedCount, failureCount);
    return failureCount == 0 ? 0 : 1;
}
