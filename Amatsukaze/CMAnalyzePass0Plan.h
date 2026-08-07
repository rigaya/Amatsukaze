#pragma once

namespace cmanalyze_plan {

// 外部exeを起動せず、CM解析とpass0前処理の実行回数を検証するための計画。
struct ExecutionPlan {
    int chapterExeCount = 0;
    int preliminaryJoinCount = 0;
    int finalJoinCount = 0;
    int preliminaryPmtApplyCount = 0;
    int finalPmtApplyCount = 0;
    bool reusesPreliminaryForPass0 = false;
};

// no-logo CM推定を一度実行したら、pass0範囲の捕捉失敗後も再実行しない。
inline bool ShouldRunNoLogoPreliminary(const bool chapterPrepared, const bool noLogoCMEstimated) {
    return chapterPrepared && !noLogoCMEstimated;
}

inline ExecutionPlan MakeExecutionPlan(const bool chapterEnabled, const bool noLogoInCM,
    const bool logoMismatchRetry, const bool pmtEnabled) {
    ExecutionPlan plan;
    if (!chapterEnabled) {
        return plan;
    }
    plan.chapterExeCount = 1;
    if (noLogoInCM) {
        plan.preliminaryJoinCount = 1;
        plan.preliminaryPmtApplyCount = pmtEnabled ? 1 : 0;
        plan.reusesPreliminaryForPass0 = true;
        return plan;
    }
    if (logoMismatchRetry) {
        plan.preliminaryJoinCount = 1;
        plan.preliminaryPmtApplyCount = pmtEnabled ? 1 : 0;
        plan.reusesPreliminaryForPass0 = true;
    }
    plan.finalJoinCount = 1;
    plan.finalPmtApplyCount = pmtEnabled ? 1 : 0;
    return plan;
}

} // namespace cmanalyze_plan
