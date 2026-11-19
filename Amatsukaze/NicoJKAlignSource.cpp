#include "NicoJK.h"
#include "NicoJKAlign.h"
#include "NicoJKAlignAudio.h"
#include "OSUtil.h"

namespace {

void WriteAlignJSON(const tstring& path, const std::string& text) {
    File file(path, _T("wb"));
    file.write(MemoryChunk((uint8_t*)text.data(), text.size()));
}

}

// 対象は1パス目のPCMを使い、基準の録画だけを追加で読み込む。
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
        const auto result = AlignNicoJKAudio(target.samples, reference.samples);
        ctx.infoF(_T("実況音声照合: 基準 %s / 対象原点PTS %.3f / 照合 %.3f秒 / 区間 %zu"), source_.ref,
                  reform_->getFirstDataPTS(), result.seconds, result.segments.size());
        for (const auto& part : result.segments) {
            ctx.infoF(_T("実況照合区間: %.3f～%.3f秒 / 基準－対象 %+.3f秒 / プローブ%d本 / 相関比%.3f"), part.start,
                      part.end, part.delta, part.probes, part.ratio);
        }
        if (!source_.refDump.empty()) {
            WriteAlignJSON(source_.refDump, NicoJKAlignResultJSON(result, target.duration, reference.duration));
        }
        if (!result.success) {
            ctx.warnF(_T("実況音声照合に失敗したため対象局へ戻ります: %s"), char_to_tstring(result.reason, CP_UTF8));
            return false;
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
        timeMapPath_ = setting_.getTmpNicoJKASSPath(NICOJK_720S) + _T(".timemap.json");
        WriteAlignJSON(timeMapPath_,
                       NicoJKTimeMapJSON(MakeNicoJKTimeMap(result.segments, target.duration, reference.duration)));
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
