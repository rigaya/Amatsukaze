#include "../../Amatsukaze/LatmToAdts.h"
#include <iostream>
#include <algorithm>

namespace {
void Check(bool condition) {
    if (!condition) throw std::runtime_error("LATM変換テストに失敗しました");
}
class Writer {
    std::vector<uint8_t> bytes;
    unsigned count = 0;
public:
    void put(unsigned value, unsigned length) {
        for (unsigned i = length; i > 0; i--) {
            if (count % 8 == 0) bytes.push_back(0);
            bytes.back() |= ((value >> (i - 1)) & 1) << (7 - count % 8);
            count++;
        }
    }
    std::vector<uint8_t> loas() {
        std::vector<uint8_t> output = { 0x56, static_cast<uint8_t>(0xe0 | (bytes.size() >> 8)), static_cast<uint8_t>(bytes.size()) };
        output.insert(output.end(), bytes.begin(), bytes.end());
        return output;
    }
};
std::vector<uint8_t> Frame(bool config, bool unsupported = false, unsigned subframes = 0) {
    Writer writer;
    writer.put(!config, 1);
    if (config) {
        writer.put(unsupported, 1); // audioMuxVersion
        writer.put(1, 1); // allStreamsSameTimeFraming
        writer.put(subframes, 6); // numSubFrames
        writer.put(0, 4); // numProgram
        writer.put(0, 3); // numLayer
        writer.put(2, 5); // AAC LC
        writer.put(3, 4); // 48kHz
        writer.put(2, 4); // ステレオ
        writer.put(0, 3); // GASpecificConfig
        writer.put(0, 3); // frameLengthType
        writer.put(255, 8); // latmBufferFullness
        writer.put(0, 2); // otherDataPresent/crcCheckPresent
    }
    writer.put(3, 8);
    writer.put(0x123456, 24);
    return writer.loas();
}
std::vector<uint8_t> Feed(LatmToAdts& parser, const std::vector<uint8_t>& data, size_t chunk) {
    std::vector<uint8_t> all, output;
    for (size_t i = 0; i < data.size(); i += chunk) {
        int64_t pts = 1234;
        parser.input(data.data() + i, std::min(chunk, data.size() - i), pts, output);
        all.insert(all.end(), output.begin(), output.end());
    }
    return all;
}
}
int main() {
    try {
        auto data = Frame(true);
        const auto repeat = Frame(false);
        data.insert(data.end(), repeat.begin(), repeat.end());
        const std::vector<uint8_t> expectedFrame = { 0xff, 0xf1, 0x4c, 0x80, 1, 0x5f, 0xfc, 0x12, 0x34, 0x56 };
        auto expected = expectedFrame;
        expected.insert(expected.end(), expectedFrame.begin(), expectedFrame.end());
        // 同期語、設定、圧縮データの全バイト位置で分割を試す。
        for (size_t chunk = 1; chunk <= data.size(); chunk++) {
            LatmToAdts parser;
            Check(Feed(parser, data, chunk) == expected);
        }
        // LOASヘッダーがPESを跨いだ場合は開始PTSと継続フラグを返す。
        {
            LatmToAdts splitParser;
            const auto frame = Frame(true);
            std::vector<uint8_t> output;
            bool continued = true;
            int64_t pts = 90000;
            splitParser.input(frame.data(), 1, pts, output, &continued);
            Check(output.empty() && !continued);
            pts = 100000;
            splitParser.input(frame.data() + 1, frame.size() - 1, pts, output, &continued);
            Check(continued && pts == 90000 && output == expectedFrame);
        }
        LatmToAdts parser;
        Check(Feed(parser, repeat, 1).empty());
        Check(Feed(parser, Frame(true), 1) == expectedFrame);
        parser.reset();
        Check(Feed(parser, repeat, 1).empty());
        parser.reset();
        Check(Feed(parser, expectedFrame, 1) == expectedFrame);
        // ADTS圧縮データ中のLOAS同期語では形式を切り替えない。
        const std::vector<uint8_t> fakeLoas = { 0x56, 0xe1, 0x02, 0x03 };
        Check(Feed(parser, fakeLoas, fakeLoas.size()) == fakeLoas);
        bool rejected = false;
        try { LatmToAdts invalid; Feed(invalid, Frame(true, true), 1); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected);
        rejected = false;
        try { LatmToAdts invalid; Feed(invalid, Frame(true, false, 1), 1); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected);
        std::cout << "LATM変換テスト成功\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
