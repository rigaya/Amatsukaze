#pragma once

#include <algorithm>

namespace logo {

// trust は「この provisional line をどこまで信用してよいか」を表す係数。
// q43 の低スコア群では旧閾値(0.55)だと補正がまったく発火しなかったため、
// 0.20 からゆるやかに立ち上げ、1.40 で飽和するようにしている。
inline double CalcResidualReweightTrust(const double provisionalConsistency) {
    return std::max(0.0, std::min(1.0, (provisionalConsistency - 0.20) / 1.20));
}

// sigma は provisional line まわりの許容幅。
// provisionalStdDiff は「回帰残差」ではなく点全体の fg-bg 差の散らばりで、
// それを基準幅にしつつ、line を信用できる点ほど狭めて外れ枝を落とす。
// ただし下限を持たせ、誤った provisional line で潰しすぎないようにする。
inline double CalcResidualReweightSigma(const double provisionalStdDiff, const double trust) {
    const double sigmaBase = std::max(6.0 / 255.0, provisionalStdDiff);
    const double sigmaShrink = 1.0 + 1.65 * trust;
    return std::max(4.0 / 255.0, sigmaBase / sigmaShrink);
}

struct LogoResidualWeightParams {
    bool enabled = false;
    double sigma = 1.0;
};

// 仮回帰線が確定した後、再重み付けの走査を始める直前に画素ごとに計算する。
inline LogoResidualWeightParams PrepareLogoResidualWeight(const double provisionalConsistency,
    const double provisionalStdDiff) {
    const double trust = CalcResidualReweightTrust(provisionalConsistency);
    LogoResidualWeightParams params;
    params.enabled = trust > 1e-4;
    if (params.enabled) params.sigma = CalcResidualReweightSigma(provisionalStdDiff, trust);
    return params;
}

// サンプル値に関係なく重みが1になる画素だけを、再走査から安全に除外する。
inline bool NeedsLogoResidualReweight(const bool lineValid, const LogoResidualWeightParams& params,
    const bool lowBgEligible) {
    return lineValid && (params.enabled || lowBgEligible);
}

} // 名前空間 logo
