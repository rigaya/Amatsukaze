#include "CMAnalyzePass0Plan.h"

#include <cstdlib>
#include <iostream>

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
    const auto noLogo = cmanalyze_plan::MakeExecutionPlan(true, true, true, true);
    ok &= Expect(noLogo.chapterExeCount == 1 && noLogo.preliminaryJoinCount == 1 && noLogo.finalJoinCount == 0
        && noLogo.preliminaryPmtApplyCount == 1 && noLogo.reusesPreliminaryForPass0
        && noLogo.logoAnalysisUsesPrePmtTrims, "noLogoInCMはJLSを重複せずロゴ収集はPMT前範囲を維持する");
    const auto matched = cmanalyze_plan::MakeExecutionPlan(true, false, false, false);
    ok &= Expect(matched.chapterExeCount == 1 && matched.preliminaryJoinCount == 0 && matched.finalJoinCount == 1
        && matched.finalPmtApplyCount == 0, "ロゴ一致は最終JLSだけを実行する");
    const auto retrySuccess = cmanalyze_plan::MakeExecutionPlan(true, false, true, true);
    ok &= Expect(retrySuccess.chapterExeCount == 1 && retrySuccess.preliminaryJoinCount == 1 && retrySuccess.finalJoinCount == 1
        && retrySuccess.preliminaryPmtApplyCount == 1 && retrySuccess.finalPmtApplyCount == 1, "retry成功はpreliminaryとfinalを各1回実行する");
    const auto retryFailure = cmanalyze_plan::MakeExecutionPlan(true, false, true, false);
    ok &= Expect(retryFailure.preliminaryJoinCount == 1 && retryFailure.finalJoinCount == 1
        && retryFailure.preliminaryPmtApplyCount == 0 && retryFailure.finalPmtApplyCount == 0, "retry失敗でも最終CM推定を維持する");
    const auto chapterDisabled = cmanalyze_plan::MakeExecutionPlan(false, false, true, true);
    ok &= Expect(chapterDisabled.chapterExeCount == 0 && chapterDisabled.preliminaryJoinCount == 0 && chapterDisabled.finalJoinCount == 0,
        "チャプター無効時にpass0の外部解析を追加しない");
    ok &= Expect(!cmanalyze_plan::ShouldRunNoLogoPreliminary(true, true),
        "noLogoInCMで範囲捕捉に失敗してもJLSを再実行しない");
    const auto captureFailure = cmanalyze_plan::MakeExecutionPlan(true, true, false, true);
    ok &= Expect(captureFailure.preliminaryJoinCount == 1 && captureFailure.finalJoinCount == 0
        && !cmanalyze_plan::ShouldRunNoLogoPreliminary(true, true),
        "noLogoInCMのrange捕捉失敗後も最終CMは成立しJLSは1回だけ");
    ok &= Expect(cmanalyze_plan::ShouldRunNoLogoPreliminary(true, false),
        "retry時は未実行のno-logo推定だけを実行する");
    ok &= Expect(!cmanalyze_plan::ShouldRunNoLogoPreliminary(false, false),
        "chapter無効時はpass0のno-logo推定を実行しない");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
