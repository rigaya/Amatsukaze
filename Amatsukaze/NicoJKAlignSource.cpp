#include "NicoJK.h"
#include "NicoJKAlignAudio.h"
#include "OSUtil.h"

// 対象と基準の音声を一時フォルダにWAVで書く。照合と対応表の作成は、nicojk_assのmakeが行う。
bool NicoJK::prepareReferenceSource(int serviceId, time_t startTime, int duration) {
    try {
        if (!reform_) {
            THROW(RuntimeException, "実況の音声照合に必要な入力情報がありません");
        }
        const auto target = ReadNicoJKTargetAudio(setting_.getWaveFilePath(), reform_->getAudioFrameList(),
                                                  reform_->getFirstDataPTS(), reform_->getInDuration() / MPEG_CLOCK_HZ);
        const auto reference = ReadNicoJKReferenceAudio(ctx, source_.ref, source_.refSid.value_or(-1));
        if (!reference.hasTime) {
            ctx.warn(_T("基準録画にTOTがありません。開始時刻0で実況コメントを扱います"));
        }

        // 基準用のfileは対象局の取得には使わない。
        mergeChannel_ = -1;
        if (source_.refMode == _T("merge")) {
            getJKNum(serviceId);
            mergeChannel_ = jknum_;
            if (mergeChannel_ <= 0) {
                ctx.warn(_T("対象局に実況チャンネルがないため基準局のコメントだけを使います"));
            }
        }
        if (source_.file.empty()) {
            getJKNum(reference.serviceId);
            if (jknum_ <= 0) {
                ctx.warn(_T("基準局に実況チャンネルがないため対象局へ戻ります"));
                return false;
            }
        } else {
            jknum_ = -1;
        }
        targetWavPath_ = setting_.getTmpNicoJKASSPath(NICOJK_720S) + _T(".target.wav");
        refWavPath_ = setting_.getTmpNicoJKASSPath(NICOJK_720S) + _T(".ref.wav");
        WriteNicoJKAlignWav(targetWavPath_, target.samples);
        WriteNicoJKAlignWav(refWavPath_, reference.samples);
        refServiceId_ = reference.serviceId;
        refStart_ = reference.firstFrameTime;
        refDuration_ = reference.duration;
        if (setting_.isUseNicoJKLog()) {
            ctx.info(_T("基準録画の指定をnicojklogより優先します"));
        }
        return true;
    } catch (Exception& error) {
        ctx.warnF(_T("実況音声照合に失敗したため対象局へ戻ります: %s"), error.message());
    } catch (const std::exception& error) {
        ctx.warnF(_T("実況音声照合に失敗したため対象局へ戻ります: %s"), char_to_tstring(error.what(), CP_UTF8));
    }
    return false;
}
