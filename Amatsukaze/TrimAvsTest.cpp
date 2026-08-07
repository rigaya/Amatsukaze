#include "TrimAvs.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using trimavs::FrameRange;

bool Expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "失敗: " << message << std::endl;
    }
    return condition;
}

bool ExpectRanges(const std::vector<FrameRange>& actual,
    const std::vector<FrameRange>& expected, const char* message) {
    return Expect(actual == expected, message);
}

bool ParseAndExpect(const std::string& text, const int numFrames,
    const std::vector<FrameRange>& expected, const char* message) {
    std::vector<FrameRange> actual;
    std::string error;
    return Expect(trimavs::ParseTrimAvs(text, numFrames, actual, error), message)
        && ExpectRanges(actual, expected, message);
}

bool ParseAndExpectFailure(const std::string& text, const int numFrames, const char* message) {
    std::vector<FrameRange> actual;
    std::string error;
    return Expect(!trimavs::ParseTrimAvs(text, numFrames, actual, error), message)
        && Expect(!error.empty(), "失敗理由が空です");
}

} // namespace

int main() {
    bool ok = true;
    ok &= ParseAndExpect("Trim(0, 99)", 300, { { 0, 100 } }, "単一区間");
    ok &= ParseAndExpect("Trim(0,99) ++ Trim(200,299)", 300,
        { { 0, 100 }, { 200, 300 } }, "複数区間");
    ok &= ParseAndExpect("MyTrim(0,99) ++ Trim(200,299)", 300,
        { { 200, 300 } }, "他のAVS関数を許容");
    ok &= ParseAndExpect("  tRiM ( -10 , 9 ) ++ TRIM( 20, 29 ) ", 100,
        { { 0, 10 }, { 20, 30 } }, "空白・大小文字・負数");
    ok &= ParseAndExpect("Trim(99,99)", 100, { { 99, 100 } }, "終端変換");
    ok &= ParseAndExpect("Trim(-20,10) ++ Trim(95,500)", 100,
        { { 0, 11 }, { 95, 100 } }, "範囲外clamp");
    ok &= ParseAndExpect("Trim(50,59) ++ Trim(0,19) ++ Trim(20,49) ++ Trim(45,69)", 100,
        { { 0, 70 } }, "sort・重複・隣接merge");
    ok &= ParseAndExpectFailure("Trim(20,19)", 100, "逆転");
    ok &= ParseAndExpectFailure("Version()", 100, "Trimなし");
    ok &= ParseAndExpectFailure("Trim(-20,-1)", 100, "clamp後に空の区間");
    ok &= ParseAndExpectFailure("Trim(0, 999999999999999999999999999999)", 100, "極端な整数");
    ok &= ParseAndExpectFailure("Trim(0, 9) ++ Trim(x, 19)", 100, "壊れたTrim混在");
    ok &= ParseAndExpectFailure("Trim(0, 9)", 0, "総フレーム数不正");

    const std::vector<FrameRange> source = { { 50, 60 }, { -20, 10 }, { 10, 50 } };
    std::string text;
    std::string error;
    ok &= Expect(trimavs::FormatTrimAvs(source, 100, text, error), "AVS出力");
    std::vector<FrameRange> reparsed;
    ok &= Expect(trimavs::ParseTrimAvs(text, 100, reparsed, error), "出力の再解析");
    ok &= ExpectRanges(reparsed, { { 0, 60 } }, "出力→再解析の往復");

    std::vector<int> legacy;
    ok &= Expect(trimavs::FrameRangesToLegacyTrims(reparsed, 100, legacy, error), "旧形式への変換");
    ok &= Expect(legacy == std::vector<int>({ 0, 60 }), "旧形式の値");
    std::vector<FrameRange> fromLegacy;
    ok &= Expect(trimavs::FrameRangesFromLegacyTrims(legacy, 100, fromLegacy, error), "旧形式からの変換");
    ok &= ExpectRanges(fromLegacy, reparsed, "旧形式変換の往復");
    ok &= Expect(!trimavs::FrameRangesFromLegacyTrims({ 0 }, 100, fromLegacy, error), "旧形式の奇数要素拒否");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
