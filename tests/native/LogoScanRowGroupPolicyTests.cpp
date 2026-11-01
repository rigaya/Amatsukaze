#include "LogoScanRowGroupPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
bool Expect(const bool condition, const char* message) {
    if (!condition) std::cerr << "失敗: " << message << std::endl;
    return condition;
}

void FillRect(std::vector<uint8_t>& binary, const int width, const int minX, const int minY, const int maxX, const int maxY) {
    for (int y = minY; y <= maxY; y++) {
        for (int x = minX; x <= maxX; x++) binary[x + y * width] = 1;
    }
}

// 強い短い縦画と、二値化で落ちやすい同じ行の細い横画を合成する。
bool TestRowBandExtend() {
    constexpr int kScoreWidth = 240;
    constexpr int kScoreHeight = 120;
    constexpr float kQuietFloor = 0.001f;
    constexpr float kWeakStroke = 0.06f;
    constexpr float kStrongStroke = 0.24f;
    constexpr float kNoisyFloor = 0.10f;
    constexpr logo::RowBandRect kDigit{ 194, 50, 5, 12 };
    constexpr int kFirstLetterX = 86;
    constexpr int kLetterStep = 14;
    constexpr int kLetterWidth = 5;
    constexpr int kStrokeY = 54;
    std::vector<float> score(kScoreWidth * kScoreHeight, kQuietFloor);
    const auto fill = [&](const int minX, const int minY, const int maxX, const int maxY, const float value) {
        for (int y = minY; y <= maxY; y++) {
            for (int x = minX; x <= maxX; x++) score[x + y * kScoreWidth] = value;
        }
    };
    const auto reset = [&]() {
        std::fill(score.begin(), score.end(), kQuietFloor);
        fill(kDigit.x, kDigit.y, kDigit.x + kDigit.w - 1, kDigit.y + kDigit.h - 1, kStrongStroke);
    };
    bool ok = true;
    reset();
    for (int x = kFirstLetterX; x < kDigit.x; x += kLetterStep) {
        fill(x, kStrokeY, x + kLetterWidth - 1, kStrokeY, kWeakStroke);
    }
    const auto original = score;
    const auto row = logo::TryExtendRowBand(score, kScoreWidth, kScoreHeight, kDigit);
    ok &= Expect(row.extended && row.minX == kFirstLetterX && row.maxX == kDigit.x + kDigit.w - 1,
        "q168相当の弱い細い文字列を左へ連鎖して拾う");
    ok &= Expect(row.floor == kQuietFloor && row.peak == kStrongStroke
        && row.threshold == logo::kRowExtendOnRatio * kStrongStroke
        && row.beforeMinX == kDigit.x && row.beforeMaxX == kDigit.x + kDigit.w - 1,
        "床・ピーク・閾値と拡張前の横範囲を返す");
    ok &= Expect(score == original, "行帯拡張は入力スコアを変更しない");

    // 行帯の上下どちらかに続く列は、その列だけでも候補から除く。
    const int bandMinY = kDigit.y - logo::kRowExtendBandPadding;
    const int bandMaxY = kDigit.y + kDigit.h - 1 + logo::kRowExtendBandPadding;
    const int bandH = bandMaxY - bandMinY + 1;
    const int nearX = kDigit.x - 2;
    for (const bool upper : { true, false }) {
        reset();
        fill(nearX, kStrokeY, nearX, kStrokeY, kWeakStroke);
        const int probeY = upper ? bandMinY - logo::kRowExtendProbeOffset : bandMaxY + logo::kRowExtendProbeOffset;
        fill(nearX, probeY, nearX, probeY, kWeakStroke);
        ok &= Expect(!logo::TryExtendRowBand(score, kScoreWidth, kScoreHeight, kDigit).extended,
            "帯の上下に続く縦構造の列は伸ばさない");
    }

    // OFFの連続数が上限に達する直前は拾い、上限以降の信号には届かない。
    const int maxOff = std::max(logo::kRowExtendMinOffColumns, bandH);
    for (const int distance : { maxOff, maxOff + 1, maxOff + 2 }) {
        reset();
        const int letterX = kDigit.x - distance;
        fill(letterX, kStrokeY, letterX, kStrokeY, kWeakStroke);
        const auto gap = logo::TryExtendRowBand(score, kScoreWidth, kScoreHeight, kDigit);
        ok &= Expect(gap.extended == (distance == maxOff), "連続OFF列がmax(6, bandH)に達したら停止");
    }
    reset();
    fill(nearX, kStrokeY, nearX, kStrokeY, kWeakStroke);
    std::replace(score.begin(), score.end(), kQuietFloor, kNoisyFloor);
    const auto noisy = logo::TryExtendRowBand(score, kScoreWidth, kScoreHeight, kDigit);
    ok &= Expect(!noisy.extended && noisy.floor == kNoisyFloor
        && noisy.threshold == logo::kRowExtendFloorMultiplier * kNoisyFloor, "床が高い画像では伸ばさない");

    // 右方向も同じ条件を使い、画面端では配列外を参照しない。
    reset();
    const int rightX = kDigit.x + kDigit.w + 2;
    fill(rightX, kStrokeY, rightX, kStrokeY, kWeakStroke);
    ok &= Expect(logo::TryExtendRowBand(score, kScoreWidth, kScoreHeight, kDigit).maxX == rightX, "右方向も最後のON列まで伸ばす");
    std::fill(score.begin(), score.end(), 0.0f);
    ok &= Expect(!logo::TryExtendRowBand(score, kScoreWidth, kScoreHeight, kDigit).extended, "無信号では伸ばさない");
    ok &= Expect(!logo::TryExtendRowBand({}, kScoreWidth, kScoreHeight, kDigit).extended, "画像サイズが不正なら拒否");
    reset();
    score.front() = std::numeric_limits<float>::quiet_NaN();
    ok &= Expect(!logo::TryExtendRowBand(score, kScoreWidth, kScoreHeight, kDigit).extended, "非有限スコアは拒否");
    constexpr logo::RowBandRect kEdgeRect{ 0, 0, 1, 1 };
    std::fill(score.begin(), score.end(), 0.0f);
    fill(0, 0, 1, 0, kStrongStroke);
    ok &= Expect(logo::TryExtendRowBand(score, kScoreWidth, kScoreHeight, kEdgeRect).maxX == 1, "画像端の行帯を安全に拡張");
    return ok;
}
}

int main() {
    bool ok = TestRowBandExtend();
    constexpr int kWidth = 100;
    constexpr int kHeight = 100;
    std::vector<uint8_t> binary(kWidth * kHeight, 0);
    // q85相当: 文字範囲から左へ28px伸びる長い1px横線を削る。
    FillRect(binary, kWidth, 30, 30, 60, 45);
    FillRect(binary, kWidth, 2, 25, 83, 25);
    const auto horizontal = logo::TrimOverhangingThinLines(binary, kWidth, kHeight);
    ok &= Expect(horizontal.removedPixels == 43 && horizontal.maxOverhang == 28, "q85相当の横線は4px許容分を残して両端を削る");
    ok &= Expect(!horizontal.binary[25 + 25 * kWidth] && horizontal.binary[26 + 25 * kWidth]
        && horizontal.binary[64 + 25 * kWidth] && !horizontal.binary[65 + 25 * kWidth], "横線のはみ出し4px境界を維持");
    ok &= Expect(horizontal.binary[30 + 30 * kWidth] && horizontal.binary[60 + 45 * kWidth], "文字本体は維持");

    std::fill(binary.begin(), binary.end(), 0);
    FillRect(binary, kWidth, 30, 30, 70, 45);
    FillRect(binary, kWidth, 30, 50, 70, 50);
    ok &= Expect(logo::TrimOverhangingThinLines(binary, kWidth, kHeight).binary == binary, "q136相当の文字幅内の下線は残す");
    // 文字に接続した細線も、上下2px位置のON率が小さければ認識する。
    std::fill(binary.begin(), binary.end(), 0);
    FillRect(binary, kWidth, 45, 20, 50, 40);
    FillRect(binary, kWidth, 2, 20, 83, 20);
    ok &= Expect(logo::TrimOverhangingThinLines(binary, kWidth, kHeight).removedPixels > 0, "文字に接続した長い横線を認識");

    std::fill(binary.begin(), binary.end(), 0);
    FillRect(binary, kWidth, 30, 30, 45, 60);
    FillRect(binary, kWidth, 25, 2, 25, 83);
    const auto vertical = logo::TrimOverhangingThinLines(binary, kWidth, kHeight);
    ok &= Expect(vertical.removedPixels == 43 && vertical.maxOverhang == 28, "垂直線も同じはみ出し条件で削る");
    std::fill(binary.begin(), binary.end(), 0);
    FillRect(binary, kWidth, 25, 2, 25, 83);
    ok &= Expect(logo::TrimOverhangingThinLines(binary, kWidth, kHeight).binary == binary, "細線しかない場合は元マスクを維持");
    std::fill(binary.begin(), binary.end(), 0);
    ok &= Expect(logo::TrimOverhangingThinLines(binary, kWidth, kHeight).binary == binary, "空マスクを維持");
    FillRect(binary, kWidth, 30, 30, 60, 45);
    FillRect(binary, kWidth, 0, 20, 22, 20);
    ok &= Expect(logo::TrimOverhangingThinLines(binary, kWidth, kHeight).binary == binary, "24px未満のランは削らない");
    std::fill(binary.begin(), binary.end(), 0);
    FillRect(binary, kWidth, 30, 30, 60, 45);
    FillRect(binary, kWidth, 26, 20, 64, 20);
    ok &= Expect(logo::TrimOverhangingThinLines(binary, kWidth, kHeight).binary == binary, "はみ出しが両端4px以内なら維持");
    FillRect(binary, kWidth, 2, 20, 83, 21);
    ok &= Expect(logo::TrimOverhangingThinLines(binary, kWidth, kHeight).removedPixels > 0, "2px幅の細線も上下2px位置で認識");
    std::fill(binary.begin(), binary.end(), 0);
    FillRect(binary, kWidth, 30, 30, 60, 45);
    FillRect(binary, kWidth, 0, 20, 29, 20);
    FillRect(binary, kWidth, 21, 22, 29, 22);
    ok &= Expect(logo::TrimOverhangingThinLines(binary, kWidth, kHeight).binary == binary, "片側のON率が0.3ちょうどなら細線扱いしない");
    binary[21 + 22 * kWidth] = 0;
    ok &= Expect(logo::TrimOverhangingThinLines(binary, kWidth, kHeight).removedPixels > 0, "両側のON率が0.3未満なら細線扱いする");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
