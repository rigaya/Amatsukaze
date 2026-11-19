#include "NicoJK.h"
#include "NicoJKAlignAudio.h"
#include "OSUtil.h"
#include <filesystem>

tstring NicoJK::getDumpDir() const {
    return setting_.getNicoJKSource().dumpDir;
}

// 調査用の置き場所があればそこ、なければ一時フォルダに置く。
tstring NicoJK::getWorkFilePath(const tchar* name) const {
    const auto dumpDir = getDumpDir();
    if (!dumpDir.empty()) {
        return (std::filesystem::path(dumpDir) / name).native();
    }
    return setting_.getTmpNicoJKASSPath(NICOJK_720S) + _T(".") + name;
}

// 対象の音声を一時フォルダにWAVで書く。照合と対応表の作成は、nicojk_assのmakeが行う。
bool NicoJK::prepareTargetAudio() {
    try {
        if (!reform_) {
            THROW(RuntimeException, "実況の音声照合に必要な入力情報がありません");
        }
        const auto target = ReadNicoJKTargetAudio(setting_.getWaveFilePath(), reform_->getAudioFrameList(),
                                                  reform_->getFirstDataPTS(), reform_->getInDuration() / MPEG_CLOCK_HZ);
        targetWavPath_ = getWorkFilePath(_T("target.wav"));
        WriteNicoJKAlignWav(targetWavPath_, target.samples);
        return true;
    } catch (Exception& error) {
        ctx.warnF(_T("実況音声の準備に失敗したため対象局へ戻ります: %s"), error.message());
    } catch (const std::exception& error) {
        ctx.warnF(_T("実況音声の準備に失敗したため対象局へ戻ります: %s"), char_to_tstring(error.what(), CP_UTF8));
    }
    targetWavPath_.clear();
    return false;
}

// 本編の区間(Trimのフレーム番号)を、対象の音声のサンプル0と同じ原点からの秒に直してJSONに書く。
// 失敗しても実況は続けられる(nicojk_assは区間なしの方法で推定する)ので、警告にとどめる。
bool NicoJK::prepareSpans() {
    spansPath_.clear();
    if (!reform_ || !mainTrims_ || mainTrims_->empty()) {
        return false;
    }
    try {
        const auto& frames = reform_->getFilterSourceFrames(0);
        if (frames.empty()) {
            return false;
        }
        const double origin = reform_->getFirstDataPTS();
        // フレーム番号fの表示開始の時刻。frames.size()は最後のフレームの終わり。
        auto toSeconds = [&](int f) {
            if (f >= (int)frames.size()) {
                return (frames.back().pts + frames.back().frameDuration - origin) / MPEG_CLOCK_HZ;
            }
            return (frames[std::max(f, 0)].pts - origin) / MPEG_CLOCK_HZ;
        };
        std::string json = "{\"spans\": [";
        bool first = true;
        for (size_t i = 0; i + 1 < mainTrims_->size(); i += 2) {
            const double start = toSeconds((*mainTrims_)[i]);
            const double end = toSeconds((*mainTrims_)[i + 1]);
            if (!(end > start)) continue;
            json += StringFormat("%s[%.6f, %.6f]", first ? "" : ", ", start, end);
            first = false;
        }
        json += "]}\n";
        if (first) {
            return false;
        }
        const auto path = getWorkFilePath(_T("spans.json"));
        File file(path, _T("wb"));
        file.write(MemoryChunk((uint8_t*)json.data(), json.size()));
        spansPath_ = path;
        return true;
    } catch (Exception& error) {
        ctx.warnF(_T("本編の区間を書けなかったため、区間なしで実況を処理します: %s"), error.message());
    }
    return false;
}
