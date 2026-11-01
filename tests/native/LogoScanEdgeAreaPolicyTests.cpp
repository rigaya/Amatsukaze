#include "LogoScanEdgeAreaPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
constexpr int kWidth = 320;
constexpr int kHeight = 180;
constexpr int kFrameWidth = 1000;
constexpr int kFrameHeight = 600;
constexpr float kQuiet = 0.01f;
constexpr float kStrong = 0.8f;
constexpr logo::EdgeAreaRect kLogo{ 40, 40, 60, 30 };

bool Expect(const bool condition, const char* message) {
    if (!condition) std::cerr << "失敗: " << message << std::endl;
    return condition;
}

std::vector<float> Image() { return std::vector<float>(kWidth * kHeight, kQuiet); }

void Fill(std::vector<float>& image, const logo::EdgeAreaRect& rect, const float value = kStrong) {
    for (int y = rect.y; y < rect.y + rect.h; y++) {
        for (int x = rect.x; x < rect.x + rect.w; x++) image[x + y * kWidth] = value;
    }
}

logo::EdgeAreaResult Pick(const std::vector<float>& image) {
    return logo::PickEdgeArea(image, kWidth, kHeight, kFrameWidth, kFrameHeight);
}

bool SameRect(const logo::EdgeAreaRect& a, const logo::EdgeAreaRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool TestLetters() {
    auto image = Image();
    constexpr int kStrokeWidth = 6;
    constexpr int kLetterStep = 18;
    constexpr int kCrossbarY = 12;
    for (int x = kLogo.x; x < kLogo.x + kLogo.w; x += kLetterStep) {
        Fill(image, { x, kLogo.y, kStrokeWidth, kLogo.h });
    }
    Fill(image, { kLogo.x, kLogo.y + kCrossbarY, kLogo.w, kStrokeWidth });
    const auto before = image;
    const auto result = Pick(image);
    return Expect(result.accepted && result.share == 1.0, "孤立した文字状の塊を採用する")
        & Expect(result.middle == kQuiet && result.limit == kStrong, "中央値と上位1%の強さを返す")
        & Expect(SameRect(result.selected, { 41, 41, 58, 28 }), "膨張4画素とSobelの1画素を戻す")
        & Expect(image == before, "入力画像を変更しない");
}

bool TestScattered() {
    auto image = Image();
    Fill(image, { 20, 20, 60, 30 });
    Fill(image, { 140, 20, 60, 30 });
    Fill(image, { 20, 110, 60, 30 });
    const auto result = Pick(image);
    return Expect(!result.accepted && result.reason == logo::EdgeAreaHoldReason::InsufficientShare
        && result.mergedStrong * 3 == result.rivals, "離れた3塊に散った強い縁は独占率不足で保留する");
}

bool TestWeakEdge() {
    constexpr float kMiddle = 0.3f;
    constexpr float kBelowTriple = 0.89f;
    auto image = std::vector<float>(kWidth * kHeight, kMiddle);
    Fill(image, kLogo, kBelowTriple);
    const auto ratio = Pick(image);
    bool ok = Expect(!ratio.accepted && ratio.reason == logo::EdgeAreaHoldReason::WeakEdge
        && ratio.middle == kMiddle && ratio.limit == kBelowTriple, "上位1%が中央値の3倍に届かなければ保留する");
    image = Image();
    Fill(image, kLogo, logo::kEdgeAreaFloor - kQuiet);
    const auto floor = Pick(image);
    ok &= Expect(!floor.accepted && floor.reason == logo::EdgeAreaHoldReason::WeakEdge, "出現率0.15未満は保留する");
    Fill(image, kLogo, logo::kEdgeAreaFloor);
    ok &= Expect(Pick(image).accepted, "出現率の下限ちょうどは採用する");
    return ok;
}

bool TestLargeBand() {
    auto image = Image();
    Fill(image, kLogo);
    constexpr logo::EdgeAreaRect kBand{ 5, 110, 300, 10 };
    Fill(image, kBand);
    const auto result = Pick(image);
    constexpr size_t kLogoStrong = kLogo.w * kLogo.h;
    return Expect(result.accepted && result.selectedStrong == kLogoStrong && result.rivals == kLogoStrong
        && result.share == 1.0 && SameRect(result.selected, { 41, 41, 58, 28 }),
        "ロゴより強いテロップの帯を候補と競争相手から除いて隣のロゴを選ぶ");
}

bool TestMergeGap() {
    // 膨張を戻した後の半開矩形間の距離を16/17画素にする。
    constexpr int kNeighborX = 114;
    constexpr int kNeighborWidth = 20;
    bool ok = true;
    for (const int extraGap : { 0, 1 }) {
        auto image = Image();
        Fill(image, kLogo);
        Fill(image, { kNeighborX + extraGap, kLogo.y, kNeighborWidth, kLogo.h });
        const auto result = Pick(image);
        ok &= Expect(result.accepted, "近隣塊の有無によらず独占率を満たすロゴを採用する");
        ok &= Expect(result.mergedStrong == size_t((kLogo.w + (extraGap == 0 ? kNeighborWidth : 0)) * kLogo.h),
            "16画素以内の塊は括り、17画素離れた塊は括らない");
        ok &= Expect(result.merged.w == (extraGap == 0 ? 92 : 58), "括った枠の外接幅を返す");
    }
    return ok;
}

bool TestTrim() {
    auto image = Image();
    constexpr logo::EdgeAreaRect kWideLogo{ 40, 40, 100, 40 };
    Fill(image, kWideLogo);
    const auto result = Pick(image);
    bool ok = Expect(result.accepted && SameRect(result.merged, { 41, 41, 98, 38 })
        && SameRect(result.rect, { 43, 41, 94, 38 }), "各辺2.5%の締めで左右2列を削り、余白を足さない");
    image = Image();
    constexpr logo::EdgeAreaRect kSquareLogo{ 40, 40, 50, 50 };
    Fill(image, kSquareLogo);
    const auto square = Pick(image);
    ok &= Expect(square.accepted && SameRect(square.merged, { 41, 41, 48, 48 })
        && SameRect(square.rect, { 42, 42, 46, 46 }), "上下左右とも各辺2.5%までを締める");
    return ok;
}

bool TestInvalidAndEmpty() {
    bool ok = Expect(!logo::PickEdgeArea({}, 0, kHeight, kFrameWidth, kFrameHeight).accepted, "空の画像を拒否する");
    auto image = Image();
    ok &= Expect(Pick(image).reason == logo::EdgeAreaHoldReason::WeakEdge, "縁のない画像は保留する");
    image.front() = std::numeric_limits<float>::quiet_NaN();
    ok &= Expect(Pick(image).reason == logo::EdgeAreaHoldReason::InvalidInput, "非有限値を拒否する");
    image = Image();
    // 上位1%は存在するが、フレーム比率に対して小さすぎる塊しかない。
    Fill(image, { 40, 40, 60, 10 });
    ok &= Expect(Pick(image).reason == logo::EdgeAreaHoldReason::NoFittingBlob, "最小高さ未満の塊しかなければ保留する");
    return ok;
}
} // 無名名前空間

int main() {
    const bool ok = TestLetters() & TestScattered() & TestWeakEdge() & TestLargeBand()
        & TestMergeGap() & TestTrim() & TestInvalidAndEmpty();
    if (ok) std::cout << "縁の塊フォールバックの全テストに成功" << std::endl;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
