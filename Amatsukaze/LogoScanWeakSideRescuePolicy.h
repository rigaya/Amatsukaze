#pragma once

#include <algorithm>
#include <cmath>

namespace logo {

// 既存矩形の側面にある弱成分を、ロゴ由来の根拠が揃う場合だけ二値化へ戻す。
struct WeakSideComponentEvidence {
    float peakScore = 0.0f;
    float meanScore = 0.0f;
    float meanRescueScore = 0.0f;
    float meanUpperGate = 0.0f;
    float meanBgVarGain = 0.0f;
    float meanPresenceGain = 0.0f;
    float meanMagGain = 0.0f;
    float meanConsistencyGain = 0.0f;
    float lowThreshold = 0.0f;
    int area = 0;
    int width = 0;
    int height = 0;
    int anchorArea = 0;
    int horizontalGap = 0;
    int verticalOverlap = 0;
    bool validAB = false;
};

enum class WeakSideComponentRescueRejectReason {
    Allowed,
    InvalidEvidence,
    ShapeOrDistance,
    Score,
    RescueScore,
    UpperGate,
    BgVarGain,
    PresenceGain,
    MagGain,
    ConsistencyGain,
};

inline WeakSideComponentRescueRejectReason GetWeakSideComponentRescueRejectReason(
    const WeakSideComponentEvidence& e) {
    const bool finiteEvidence = std::isfinite(e.peakScore)
        && std::isfinite(e.meanScore)
        && std::isfinite(e.meanRescueScore)
        && std::isfinite(e.meanUpperGate)
        && std::isfinite(e.meanBgVarGain)
        && std::isfinite(e.meanPresenceGain)
        && std::isfinite(e.meanMagGain)
        && std::isfinite(e.meanConsistencyGain)
        && std::isfinite(e.lowThreshold);
    if (!finiteEvidence || !e.validAB || e.area < 3 || e.width <= 0 || e.height <= 0 || e.anchorArea <= 0
        || e.lowThreshold <= 0.0f || e.horizontalGap < 0 || e.verticalOverlap < 1) {
        return WeakSideComponentRescueRejectReason::InvalidEvidence;
    }
    const float aspect = std::max((float)e.width / e.height, (float)e.height / e.width);
    // アンカー本体に対して大きすぎる塊は、側面の細線ではなく別ロゴ・背景成分とみなす。
    if (aspect < 3.0f || aspect > 16.0f || e.area > 256 || e.horizontalGap > 16
        || (e.anchorArea > 0 && e.area * 8 > e.anchorArea)) {
        return WeakSideComponentRescueRejectReason::ShapeOrDistance;
    }
    // 通常lowMaskから漏れた成分だけを対象にし、各ゲートが単独で判定を通さないようにする。
    if (e.meanScore < std::max(0.12f, e.lowThreshold * 0.78f)
        || e.peakScore < e.lowThreshold * 0.82f) {
        return WeakSideComponentRescueRejectReason::Score;
    }
    if (e.meanRescueScore < std::max(0.08f, e.lowThreshold * 0.45f)) {
        return WeakSideComponentRescueRejectReason::RescueScore;
    }
    if (e.meanUpperGate < 0.85f) return WeakSideComponentRescueRejectReason::UpperGate;
    if (e.meanBgVarGain < 0.80f) return WeakSideComponentRescueRejectReason::BgVarGain;
    if (e.meanPresenceGain < 0.45f) return WeakSideComponentRescueRejectReason::PresenceGain;
    if (e.meanMagGain < 0.05f) return WeakSideComponentRescueRejectReason::MagGain;
    if (e.meanConsistencyGain < 0.20f) return WeakSideComponentRescueRejectReason::ConsistencyGain;
    return WeakSideComponentRescueRejectReason::Allowed;
}

inline bool IsWeakSideComponentRescueAllowed(const WeakSideComponentEvidence& e) {
    return GetWeakSideComponentRescueRejectReason(e)
        == WeakSideComponentRescueRejectReason::Allowed;
}

} // namespace logo
