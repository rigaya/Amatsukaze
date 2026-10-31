#include "LogoScan.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {
bool Expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "失敗: " << message << std::endl;
    }
    return condition;
}
}

int main() {
    bool ok = true;

    std::vector<double> flat(8 * 8, 0.25);
    ok &= Expect(logo::CalcRenderedYSpatialDetail(flat.data(), 8, 8, 1.0) == 0.0,
        "一様な renderedY はディテールゼロ");

    std::vector<double> checker(8 * 8, 0.0);
    for (int y = 2; y < 6; y++) {
        for (int x = 2; x < 6; x++) {
            checker[x + y * 8] = ((x + y) & 1) ? 0.0 : 0.25;
        }
    }
    const double checkerDetail = logo::CalcRenderedYSpatialDetail(checker.data(), 8, 8, 16.0 / 64.0);
    ok &= Expect(checkerDetail > 0.035, "細かい構造はディテールを持つ");

    std::vector<double> checkerDim = checker;
    for (double& value : checkerDim) {
        value *= 0.25;
    }
    const double checkerDimDetail = logo::CalcRenderedYSpatialDetail(
        checkerDim.data(), 8, 8, 16.0 / 64.0);
    ok &= Expect(checkerDimDetail < checkerDetail && checkerDimDetail > 0.0,
        "rawディテールは明るさに比例する");
    ok &= Expect(std::abs(
        logo::NormalizeRenderedYSpatialDetail(checkerDetail, 0.25)
        - logo::NormalizeRenderedYSpatialDetail(checkerDimDetail, 0.0625)) < 1.0e-12,
        "輝度正規化ディテールは明るさに依存しない");

    ok &= Expect(!logo::IsNoLogoSignal(0.01272, 0.01913, 0.42465, 8.49296),
        "q177相当の低濃度でも輪郭構造があれば許可");
    ok &= Expect(logo::IsNoLogoSignal(0.00112, 0.00432, 0.0010, 0.020),
        "q59相当の極小信号はno_logoとして拒否");
    ok &= Expect(logo::IsNoLogoSignal(0.00025, 0.0, 0.0, 0.0),
        "完全に近い無信号はno_logoとして拒否");
    ok &= Expect(logo::IsNoLogoSignal(0.01, 0.015, 0.10, 1.0),
        "低濃度構造信号の境界値はno_logoとして拒否");
    ok &= Expect(!logo::IsNoLogoSignal(0.01 + 1.0e-9, 0.015 + 1.0e-9,
        0.10 + 1.0e-9, 1.0 + 1.0e-9),
        "低濃度構造信号の閾値超過は許可");
    ok &= Expect(!logo::IsNoLogoSignal(0.02, 0.019, 0.0, 0.0),
        "no_logo 面積境界値は拒否しない");
    ok &= Expect(!logo::IsNoLogoSignal(0.019, 0.02, 0.0, 0.0),
        "no_logo renderedY 境界値は拒否しない");
    ok &= Expect(!logo::IsNoLogoSignal(0.14773, 0.21636, 0.01452, 0.06713),
        "q74相当のぼんやり信号はno_logoとしては拒否しない");
    ok &= Expect(!logo::IsNoLogoSignal(0.52545, 0.15857, 0.02044, 0.12887),
        "q112相当の色塊信号はno_logoとしては拒否しない");

    std::vector<double> padded(12 * 12, 0.0);
    for (int y = 4; y < 8; y++) {
        for (int x = 4; x < 8; x++) {
            padded[x + y * 12] = checker[(x - 2) + (y - 2) * 8];
        }
    }
    const double paddedDetail = logo::CalcRenderedYSpatialDetail(
        padded.data(), 12, 12, 16.0 / 144.0);
    ok &= Expect(std::abs(paddedDetail - checkerDetail) < 1.0e-12,
        "外周パディングを追加してもディテールは変わらない");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    ok &= Expect(logo::IsNoLogoSignal(nan, 0.01, 0.01, 0.3),
        "no_logo active area NaN は拒否");
    ok &= Expect(logo::IsNoLogoSignal(0.01, inf, 0.01, 0.3),
        "no_logo renderedY p99 Inf は拒否");
    ok &= Expect(logo::IsNoLogoSignal(0.01, 0.01, nan, 0.3),
        "no_logo rawディテール NaN は拒否");
    ok &= Expect(logo::IsNoLogoSignal(0.01, 0.01, 0.01, inf),
        "no_logo 正規化ディテール Inf は拒否");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
