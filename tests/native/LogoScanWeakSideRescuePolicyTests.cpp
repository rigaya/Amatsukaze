#include "LogoScanWeakSideRescuePolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

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
    logo::WeakSideComponentEvidence q176{};
    q176.peakScore = 0.25f;
    q176.meanScore = 0.20f;
    q176.meanRescueScore = 0.16f;
    q176.meanUpperGate = 0.96f;
    q176.meanBgVarGain = 0.98f;
    q176.meanPresenceGain = 0.95f;
    q176.meanMagGain = 0.12f;
    q176.meanConsistencyGain = 0.80f;
    q176.lowThreshold = 0.236f;
    q176.area = 16;
    q176.width = 1;
    q176.height = 16;
    q176.anchorArea = 468;
    q176.horizontalGap = 1;
    q176.verticalOverlap = 18;
    q176.validAB = true;
    ok &= Expect(logo::IsWeakSideComponentRescueAllowed(q176),
        "q176相当の近接弱成分を救済");
    auto horizontalLine = q176;
    horizontalLine.area = 14;
    horizontalLine.width = 14;
    horizontalLine.height = 1;
    horizontalLine.verticalOverlap = 1;
    ok &= Expect(logo::IsWeakSideComponentRescueAllowed(horizontalLine),
        "アンカー帯と1行だけ重なる水平輪郭を救済");
    auto outsideLine = q176;
    outsideLine.verticalOverlap = 0;
    ok &= Expect(!logo::IsWeakSideComponentRescueAllowed(outsideLine),
        "アンカー帯から外れた下側水平線を救済しない");

    auto weak = q176;
    weak.meanBgVarGain = 0.79f;
    ok &= Expect(!logo::IsWeakSideComponentRescueAllowed(weak),
        "背景分散ゲイン不足の成分を救済しない");
    weak = q176;
    weak.horizontalGap = 17;
    ok &= Expect(!logo::IsWeakSideComponentRescueAllowed(weak),
        "アンカーから遠い側面成分を救済しない");
    weak = q176;
    weak.meanRescueScore = 0.05f;
    ok &= Expect(!logo::IsWeakSideComponentRescueAllowed(weak),
        "rescue根拠が弱い成分を救済しない");
    weak = q176;
    weak.validAB = false;
    ok &= Expect(!logo::IsWeakSideComponentRescueAllowed(weak),
        "回帰不成立の成分をこの経路では救済しない");
    weak = q176;
    weak.area = 15;
    weak.width = 6;
    weak.height = 7;
    weak.anchorArea = 167;
    ok &= Expect(!logo::IsWeakSideComponentRescueAllowed(weak),
        "縦横比の低いcompact blobを救済しない");
    weak = q176;
    weak.area = 27;
    weak.width = 10;
    weak.height = 9;
    weak.anchorArea = 1510;
    ok &= Expect(!logo::IsWeakSideComponentRescueAllowed(weak),
        "面積比が小さくてもcompact blobを救済しない");
    weak = q176;
    weak.anchorArea = 0;
    ok &= Expect(!logo::IsWeakSideComponentRescueAllowed(weak),
        "アンカー面積がない成分を救済しない");
    weak = q176;
    weak.meanScore = std::numeric_limits<float>::quiet_NaN();
    ok &= Expect(!logo::IsWeakSideComponentRescueAllowed(weak),
        "NaNを含む証拠を救済しない");
    weak = q176;
    weak.meanRescueScore = std::numeric_limits<float>::infinity();
    ok &= Expect(!logo::IsWeakSideComponentRescueAllowed(weak),
        "Infを含む証拠を救済しない");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
