/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "TranscodeManager.h"
#include "rgy_util.h"
#include "rgy_thread_affinity.h"
#include <thread>
#include "AdtsParser.h"
#include "PacketCache.h"
#include "rgy_pipe.h"
#include "rgy_mutex.h"
#include "Subtitle.h"
#include "WaveWriter.h"
#include <filesystem>

namespace {

// tsreadexの実行ファイルが利用可能かを判定する（ファイル存在 or PATH上に存在）
static bool isTsReadExAvailable(const ConfigWrapper& setting) {
    const auto& path = setting.getTsReadExPath();
    return !path.empty()
        && (rgy_file_exists(path) || !find_executable_in_path(path).empty());
}

struct WhisperAudioEntry {
    int keyIndex;
    EncodeFileKey key;
    int localIndex;
    tstring audioPath;
    int audioSourceIndex;
    int dualMonoChannel; // -1: original stereo, 0/1: dual mono channel selection
};

constexpr int RESUME_MANIFEST_VERSION = 2;

struct ResumeVideoInfo {
    int numFrames;
    tstring logoPath;
    std::vector<int> trims;
    std::vector<int> divs;
};

struct ResumeInfo {
    int64_t srcFileSize;
    int64_t srcWriteTime;
    int requestedServiceId;
    bool captionsParsed;
    bool tsreplace;
    bool chapter;
    bool pmtCut;
    double pmtCutSideRate[2];
    std::vector<tstring> logoPath;
    std::vector<tstring> eraseLogoPath;
    bool ignoreNoLogo;
    bool noDelogo;
    bool looseLogoDetection;
    int autoLogoDetect;
    int autoLogoDetectSearchFrames;
    int autoLogoDetectDivX;
    int autoLogoDetectDivY;
    int autoLogoDetectBlockSize;
    int autoLogoDetectThreshold;
    int autoLogoDetectMarginX;
    int autoLogoDetectMarginY;
    int serviceId;
    int64_t numTotalPackets;
    int64_t numScramblePackets;
    int64_t totalIntVideoSize;
    int64_t splitterSrcFileSize;
    int noDrcsMapCount;
    std::vector<ResumeVideoInfo> videos;
};

static void writeTString(const File& file, const tstring& str) {
    file.writeArray(std::vector<tchar>(str.begin(), str.end()));
}

static void writeTStringArray(const File& file, const std::vector<tstring>& strings) {
    file.writeValue((int64_t)strings.size());
    for (const auto& str : strings) {
        writeTString(file, str);
    }
}

static tstring readTString(const File& file) {
    const auto chars = file.readArray<tchar>();
    return tstring(chars.begin(), chars.end());
}

static std::vector<tstring> readTStringArray(const File& file) {
    const auto count = file.readValue<int64_t>();
    if (count < 0 || count > INT_MAX) {
        THROW(FormatException, "再開情報の文字列配列数が不正です");
    }
    std::vector<tstring> strings;
    strings.reserve((size_t)count);
    for (int64_t i = 0; i < count; i++) {
        strings.push_back(readTString(file));
    }
    return strings;
}

static int64_t getFileWriteTime(const tstring& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(std::filesystem::path(path), error);
    if (error) {
        THROWF(IOException, "入力ファイルの更新時刻を取得できません: %s", path.c_str());
    }
    return static_cast<int64_t>(time.time_since_epoch().count());
}

static int64_t getFileSize(const tstring& path) {
    File file(path, _T("rb"));
    return file.size();
}

static void saveResumeManifest(
    const ConfigWrapper& setting,
    StreamReformInfo& reformInfo,
    const std::vector<std::unique_ptr<CMAnalyze>>& cmanalyze,
    const int serviceId,
    const int64_t numTotalPackets,
    const int64_t numScramblePackets,
    const int64_t totalIntVideoSize,
    const int64_t srcFileSize,
    const int noDrcsMapCount,
    const bool captionsParsed) {
    const auto srcPath = setting.getSrcFilePath();
    const auto pmtCutSideRate = setting.getPmtCutSideRate();
    File file(setting.getTmpResumePath(), _T("wb"));

    file.writeValue(RESUME_MANIFEST_VERSION);
    file.writeValue(srcFileSize);
    file.writeValue(getFileWriteTime(srcPath));

    file.writeValue(setting.getServiceId());
    file.writeValue(captionsParsed);
    file.writeValue(setting.getFormat() == FORMAT_TSREPLACE);
    file.writeValue(setting.isChapterEnabled());
    file.writeValue(setting.isPmtCutEnabled());
    file.writeValue(pmtCutSideRate[0]);
    file.writeValue(pmtCutSideRate[1]);
    writeTStringArray(file, setting.getLogoPath());
    writeTStringArray(file, setting.getEraseLogoPath());
    file.writeValue(setting.isIgnoreNoLogo());
    file.writeValue(setting.isNoDelogo());
    file.writeValue(setting.isLooseLogoDetection());
    file.writeValue(setting.getAutoLogoDetect());
    file.writeValue(setting.getAutoLogoDetectSearchFrames());
    file.writeValue(setting.getAutoLogoDetectDivX());
    file.writeValue(setting.getAutoLogoDetectDivY());
    file.writeValue(setting.getAutoLogoDetectBlockSize());
    file.writeValue(setting.getAutoLogoDetectThreshold());
    file.writeValue(setting.getAutoLogoDetectMarginX());
    file.writeValue(setting.getAutoLogoDetectMarginY());

    file.writeValue(serviceId);
    file.writeValue(numTotalPackets);
    file.writeValue(numScramblePackets);
    file.writeValue(totalIntVideoSize);
    file.writeValue(srcFileSize);
    file.writeValue(noDrcsMapCount);

    file.writeValue((int)cmanalyze.size());
    for (int videoFileIndex = 0; videoFileIndex < (int)cmanalyze.size(); videoFileIndex++) {
        const auto& cma = cmanalyze[videoFileIndex];
        const int numFrames = (int)reformInfo.getFilterSourceFrames(videoFileIndex).size();
        file.writeValue(numFrames);
        writeTString(file, cma->getLogoPath());
        file.writeArray(cma->getTrims());
        file.writeArray(cma->getDivs());
    }
}

static ResumeInfo readResumeManifest(const tstring& path) {
    File file(path, _T("rb"));
    const auto version = file.readValue<int>();
    if (version != RESUME_MANIFEST_VERSION) {
        THROWF(FormatException, "再開情報のバージョンが未対応です: %d", version);
    }

    ResumeInfo info;
    info.srcFileSize = file.readValue<int64_t>();
    info.srcWriteTime = file.readValue<int64_t>();
    info.requestedServiceId = file.readValue<int>();
    info.captionsParsed = file.readValue<bool>();
    info.tsreplace = file.readValue<bool>();
    info.chapter = file.readValue<bool>();
    info.pmtCut = file.readValue<bool>();
    info.pmtCutSideRate[0] = file.readValue<double>();
    info.pmtCutSideRate[1] = file.readValue<double>();
    info.logoPath = readTStringArray(file);
    info.eraseLogoPath = readTStringArray(file);
    info.ignoreNoLogo = file.readValue<bool>();
    info.noDelogo = file.readValue<bool>();
    info.looseLogoDetection = file.readValue<bool>();
    info.autoLogoDetect = file.readValue<int>();
    info.autoLogoDetectSearchFrames = file.readValue<int>();
    info.autoLogoDetectDivX = file.readValue<int>();
    info.autoLogoDetectDivY = file.readValue<int>();
    info.autoLogoDetectBlockSize = file.readValue<int>();
    info.autoLogoDetectThreshold = file.readValue<int>();
    info.autoLogoDetectMarginX = file.readValue<int>();
    info.autoLogoDetectMarginY = file.readValue<int>();
    info.serviceId = file.readValue<int>();
    info.numTotalPackets = file.readValue<int64_t>();
    info.numScramblePackets = file.readValue<int64_t>();
    info.totalIntVideoSize = file.readValue<int64_t>();
    info.splitterSrcFileSize = file.readValue<int64_t>();
    info.noDrcsMapCount = file.readValue<int>();

    const auto videoCount = file.readValue<int>();
    if (videoCount < 0 || videoCount > INT_MAX) {
        THROW(FormatException, "再開情報の映像数が不正です");
    }
    info.videos.resize(videoCount);
    for (auto& video : info.videos) {
        video.numFrames = file.readValue<int>();
        video.logoPath = readTString(file);
        video.trims = file.readArray<int>();
        video.divs = file.readArray<int>();
    }
    return info;
}

static bool validateResumeSetting(const ConfigWrapper& setting, const ResumeInfo& info, tstring& reason) {
    std::vector<tstring> mismatchedFields;
    if (info.srcFileSize != getFileSize(setting.getSrcFilePath())
        || info.srcWriteTime != getFileWriteTime(setting.getSrcFilePath())) {
        mismatchedFields.push_back(_T("入力TS"));
    }
    if (info.requestedServiceId != setting.getServiceId()) {
        mismatchedFields.push_back(_T("サービスID"));
    }
    if (info.tsreplace != (setting.getFormat() == FORMAT_TSREPLACE)) {
        mismatchedFields.push_back(_T("TS置換出力"));
    }
    if (info.eraseLogoPath != setting.getEraseLogoPath()) {
        mismatchedFields.push_back(_T("追加ロゴ消し設定"));
    }
    if (mismatchedFields.empty()) {
        return true;
    }

    reason = _T("再開情報と設定が一致しません(");
    for (int index = 0; index < (int)mismatchedFields.size(); index++) {
        if (index > 0) {
            reason += _T(", ");
        }
        reason += mismatchedFields[index];
    }
    reason += _T(")");
    return false;
}

static bool validateResumeFiles(
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    const ResumeInfo& info,
    tstring& reason) {
    const int numVideoFiles = reformInfo.getNumVideoFile();
    if ((int)info.videos.size() != numVideoFiles) {
        reason = _T("再開情報の映像数が一致しません");
        return false;
    }
    if (!File::exists(setting.getAudioFilePath()) || !File::exists(setting.getWaveFilePath())) {
        reason = _T("再開に必要な音声一時ファイルがありません");
        return false;
    }
    if (isTsReadExAvailable(setting) && !File::exists(setting.getTmpTsReadExDumpPath())) {
        reason = _T("再開に必要なtsreadex_dump.txtがありません");
        return false;
    }

    for (int videoFileIndex = 0; videoFileIndex < numVideoFiles; videoFileIndex++) {
        const auto& video = info.videos[videoFileIndex];
        const int numFrames = (int)reformInfo.getFilterSourceFrames(videoFileIndex).size();
        if (video.numFrames != numFrames) {
            reason = StringFormat(_T("再開情報のフレーム数が一致しません: %d"), videoFileIndex);
            return false;
        }
        if (video.trims.size() % 2 != 0) {
            reason = StringFormat(_T("再開情報のTrim区間数が不正です: %d"), videoFileIndex);
            return false;
        }
        for (int i = 0; i < (int)video.trims.size(); i += 2) {
            if (video.trims[i] < 0 || video.trims[i + 1] < video.trims[i]
                || video.trims[i + 1] > numFrames) {
                reason = StringFormat(_T("再開情報のTrim区間が不正です: %d"), videoFileIndex);
                return false;
            }
        }
        if (video.divs.size() > 0) {
            if (video.divs.size() < 2 || video.divs.front() != 0 || video.divs.back() != numFrames) {
                reason = StringFormat(_T("再開情報の分割情報が不正です: %d"), videoFileIndex);
                return false;
            }
            for (int i = 1; i < (int)video.divs.size(); i++) {
                if (video.divs[i] < video.divs[i - 1]) {
                    reason = StringFormat(_T("再開情報の分割順序が不正です: %d"), videoFileIndex);
                    return false;
                }
            }
        }
        const bool requiresSavedLogo = !setting.isNoDelogo()
            && setting.getLogoPath().size() > 0
            && numFrames >= 300;
        if (requiresSavedLogo && video.logoPath.empty()) {
            reason = StringFormat(_T("ロゴ消しに必要な保存済みロゴ情報がありません: %d"), videoFileIndex);
            return false;
        }
        if (!File::exists(setting.getIntVideoFilePath(videoFileIndex))
            || !File::exists(setting.getTmpAMTSourcePath(videoFileIndex))) {
            reason = StringFormat(_T("再開に必要な映像一時ファイルがありません: %d"), videoFileIndex);
            return false;
        }
        if (video.divs.size() > 0 && !File::exists(setting.getTmpTrimAVSPath(videoFileIndex))) {
            reason = StringFormat(_T("再開に必要なTrimファイルがありません: %d"), videoFileIndex);
            return false;
        }
        if (setting.isChapterEnabled() && numFrames >= 300
            && !File::exists(setting.getTmpJlsPath(videoFileIndex))) {
            reason = StringFormat(_T("再開に必要なチャプター情報がありません: %d"), videoFileIndex);
            return false;
        }
        if (video.logoPath.size() > 0 && !File::exists(video.logoPath)) {
            reason = StringFormat(_T("再開に必要なロゴファイルがありません: %d"), videoFileIndex);
            return false;
        }
    }
    return true;
}

static bool tryLoadResume(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    ResumeInfo& info,
    std::unique_ptr<StreamReformInfo>& reformInfo) {
    if (setting.getResumeDir().size() == 0) {
        return false;
    }
    if (!rgy_path_is_same(setting.getTmpDir(), setting.getResumeDir())) {
        ctx.warnF(_T("[一時ファイル再利用] 指定された再開フォルダを使用できないため通常処理へ戻ります: %s"), setting.getResumeDir().c_str());
        return false;
    }

    try {
        tstring reason;
        ctx.info(_T("[一時ファイル再利用] 再開情報を検証します"));
        if (!File::exists(setting.getTmpStreamInfoPath()) || !File::exists(setting.getTmpResumePath())) {
            ctx.warn(_T("[一時ファイル再利用] 再開情報が見つからないため通常処理へ戻ります"));
            return false;
        }
        info = readResumeManifest(setting.getTmpResumePath());
        ctx.info(_T("[一時ファイル再利用] 再開マニフェストを読み込みました"));
        if (!validateResumeSetting(setting, info, reason)) {
            ctx.warnF(_T("[一時ファイル再利用] %s。通常処理へ戻ります"), reason.c_str());
            return false;
        }
        if (setting.isSubtitlesEnabled() && !info.captionsParsed) {
            ctx.warn(_T("[一時ファイル再利用] 再開情報に字幕解析結果が含まれていないため通常処理へ戻ります"));
            return false;
        }
        reformInfo = std::make_unique<StreamReformInfo>(StreamReformInfo::deserialize(ctx, setting.getTmpStreamInfoPath()));
        if (!setting.isSubtitlesEnabled()) {
            reformInfo->clearCaptionItems();
        }
        reformInfo->prepare(setting.isSplitSub(), setting.isEncodeAudio(), setting.getFormat() == FORMAT_TSREPLACE);
        ctx.info(_T("[一時ファイル再利用] ストリーム情報を読み込みました"));
        if (!validateResumeFiles(setting, *reformInfo, info, reason)) {
            ctx.warnF(_T("[一時ファイル再利用] %s。通常処理へ戻ります"), reason.c_str());
            reformInfo.reset();
            return false;
        }
        ctx.infoF(_T("[一時ファイル再利用] 再開情報を読み込みました: %s"), setting.getTmpResumePath().c_str());
        return true;
    } catch (const Exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] 再開情報の検証に失敗したため通常処理へ戻ります: %s"), e.message());
    } catch (const std::exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] 再開情報の検証に失敗したため通常処理へ戻ります: %s"), char_to_tstring(e.what()));
    }
    reformInfo.reset();
    return false;
}

static void saveResumeFiles(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    StreamReformInfo& reformInfo,
    const std::vector<std::unique_ptr<CMAnalyze>>& cmanalyze,
    const int serviceId,
    const int64_t numTotalPackets,
    const int64_t numScramblePackets,
    const int64_t totalIntVideoSize,
    const int64_t srcFileSize,
    const int noDrcsMapCount,
    const bool captionsParsed) {
    try {
        saveResumeManifest(setting, reformInfo, cmanalyze,
            serviceId, numTotalPackets, numScramblePackets, totalIntVideoSize, srcFileSize,
            noDrcsMapCount, captionsParsed);
        ctx.infoF(_T("[一時ファイル再利用] 再開情報を保存しました: %s"), setting.getTmpResumePath().c_str());
    } catch (const Exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] 再開情報の保存に失敗しました: %s"), e.message());
    } catch (const std::exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] 再開情報の保存に失敗しました: %s"), char_to_tstring(e.what()));
    }
}

static void saveResumeStreamInfo(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    StreamReformInfo& reformInfo) {
    try {
        reformInfo.serialize(setting.getTmpStreamInfoPath());
        ctx.infoF(_T("[一時ファイル再利用] ストリーム情報を保存しました: %s"), setting.getTmpStreamInfoPath().c_str());
    } catch (const Exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] ストリーム情報の保存に失敗しました: %s"), e.message());
    } catch (const std::exception& e) {
        ctx.warnF(_T("[一時ファイル再利用] ストリーム情報の保存に失敗しました: %s"), char_to_tstring(e.what()));
    }
}

static bool isCaptionParsingEnabled(const ConfigWrapper& setting) {
    return setting.isSubtitlesEnabled() || setting.isNoRemoveTmp();
}

static void copyTrimAVSForCMOnly(
    AMTContext& ctx,
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo,
    const int numVideoFiles) {
    // trimn.avsが複数ある場合、動画時間(フレーム数)が長いものを選択してtrim.avsとしてコピー
    int bestIndex = -1;
    int bestFrames = -1;
    tstring bestSrcTrim;
    for (int vindex = 0; vindex < numVideoFiles; vindex++) {
        const auto srcTrim = setting.getTmpTrimAVSPath(vindex);
        if (!File::exists(srcTrim)) {
            continue;
        }
        const int numFrames = (int)reformInfo.getFilterSourceFrames(vindex).size();
        if (bestIndex < 0 || numFrames > bestFrames) {
            bestIndex = vindex;
            bestFrames = numFrames;
            bestSrcTrim = srcTrim;
        }
    }
    if (bestIndex >= 0) {
        const auto dstTrim = StringFormat(_T("%s.trim.avs"), setting.getSrcFileOriginalPath().c_str());
        ctx.infoF(_T("[CM解析のみ] trim%d.avs をtrim.avsとしてコピー: %s"),
            bestIndex, dstTrim.c_str());
        if (!rgy_file_copy(bestSrcTrim, dstTrim, true)) {
            ctx.warnF(_T("[CM解析のみ] trim.avsのコピーに失敗: %s -> %s"),
                bestSrcTrim.c_str(), dstTrim.c_str());
        }
    } else {
        ctx.warn(_T("[CM解析のみ] コピー対象のtrim*.avsが見つかりませんでした"));
    }
}

static tstring createWhisperWaveInput(AMTContext& ctx,
                                      const ConfigWrapper& setting,
                                      const StreamReformInfo& reformInfo,
                                      const WhisperAudioEntry& entry) {
    const int bytesPerSample = 2;
    const int srcChannels = 2; // AMTSplitter outputs 16bit stereo PCM for whisper input
    const int destChannels = (entry.dualMonoChannel >= 0) ? 1 : srcChannels;

    if (entry.audioSourceIndex < 0) {
        ctx.warn(_T("Whisper入力wav作成: 音声インデックスが不正です"));
        return tstring();
    }

    const auto& key = entry.key;
    const auto& fileIn = reformInfo.getEncodeFile(key);
    if (entry.audioSourceIndex >= (int)fileIn.audioFrames.size()) {
        ctx.warnF(_T("Whisper入力wav作成: 音声%d-%dで音声インデックス%dが範囲外です"), key.video, key.format, entry.audioSourceIndex);
        return tstring();
    }

    const auto& frameIndexList = fileIn.audioFrames[entry.audioSourceIndex];
    if (frameIndexList.empty()) {
        ctx.warnF(_T("Whisper入力wav作成: 音声%d-%d-%dにフレームが存在しません"), key.video, key.format, entry.audioSourceIndex);
        return tstring();
    }

    auto waveFrames = reformInfo.getWaveInput(frameIndexList);
    if (waveFrames.empty()) {
        ctx.warnF(_T("Whisper入力wav作成: 音声%d-%d-%dのwave情報が取得できません"), key.video, key.format, entry.audioSourceIndex);
        return tstring();
    }

    int samplesPerFrame = 0;
    for (const auto& frame : waveFrames) {
        if (frame.waveLength > 0) {
            samplesPerFrame = (int)(frame.waveLength / (bytesPerSample * srcChannels));
            break;
        }
    }
    if (samplesPerFrame == 0) {
        samplesPerFrame = 1024;
    }

    int64_t totalSamples = 0;
    for (const auto& frame : waveFrames) {
        int frameSamples = (frame.waveLength > 0)
            ? (int)(frame.waveLength / (bytesPerSample * srcChannels))
            : samplesPerFrame;
        totalSamples += frameSamples;
    }

    const auto fmt = reformInfo.getFormat(key);
    int sampleRate = 48000;
    if (entry.audioSourceIndex < (int)fmt.audioFormat.size() && fmt.audioFormat[entry.audioSourceIndex].sampleRate > 0) {
        sampleRate = fmt.audioFormat[entry.audioSourceIndex].sampleRate;
    }

    const tstring wavPath = setting.getTmpWhisperWavPath(key, entry.localIndex);
    std::unique_ptr<FILE, decltype(&fclose)> fp(_tfopen(wavPath.c_str(), _T("wb")), &fclose);
    if (!fp) {
        ctx.warnF(_T("Whisper入力wav作成: ファイルを開けません (%s)"), wavPath.c_str());
        return tstring();
    }

    writeWaveHeader(fp.get(), destChannels, sampleRate, bytesPerSample * 8, totalSamples);

    File sourceWave(setting.getWaveFilePath(), _T("rb"));
    std::vector<uint8_t> srcBuffer;
    std::vector<uint8_t> dstBuffer;

    for (const auto& frame : waveFrames) {
        int frameSamples = (frame.waveLength > 0)
            ? (int)(frame.waveLength / (bytesPerSample * srcChannels))
            : samplesPerFrame;
        if (frameSamples <= 0) {
            continue;
        }

        size_t srcBytes = (size_t)frameSamples * bytesPerSample * srcChannels;
        srcBuffer.assign(srcBytes, 0);
        if (frame.waveLength > 0) {
            sourceWave.seek(frame.waveOffset, SEEK_SET);
            MemoryChunk chunk(srcBuffer.data(), frame.waveLength);
            if (frame.waveLength > 0) {
                sourceWave.read(chunk);
            }
            if ((size_t)frame.waveLength < srcBytes) {
                std::fill(srcBuffer.begin() + frame.waveLength, srcBuffer.end(), 0);
            }
        }

        if (destChannels == srcChannels) {
            if (fwrite(srcBuffer.data(), srcBytes, 1, fp.get()) != 1) {
                THROWF(IOException, "Whisper入力wav作成: 書き込みに失敗 (%s)", wavPath.c_str());
            }
        } else {
            dstBuffer.resize((size_t)frameSamples * bytesPerSample);
            const int channelIndex = (entry.dualMonoChannel >= 0 && entry.dualMonoChannel < srcChannels) ? entry.dualMonoChannel : 0;
            const int16_t* srcSamples = reinterpret_cast<const int16_t*>(srcBuffer.data());
            int16_t* dstSamples = reinterpret_cast<int16_t*>(dstBuffer.data());
            for (int s = 0; s < frameSamples; s++) {
                dstSamples[s] = srcSamples[s * srcChannels + channelIndex];
            }
            if (fwrite(dstBuffer.data(), dstBuffer.size(), 1, fp.get()) != 1) {
                THROWF(IOException, "Whisper入力wav作成: 書き込みに失敗 (%s)", wavPath.c_str());
            }
        }
    }

    return wavPath;
}

// TS解析中にtsreadexへ入力TSを転送し、トレースを保存する。
// 解析側で例外が発生した場合も、デストラクタで標準入力を閉じて完了を待つ。
class TsReadExPipe {
    class TraceProcess : public EventBaseSubProcess {
    public:
        TraceProcess(const tstring& args, File* output)
            : EventBaseSubProcess(args)
            , output_(output) {}

    protected:
        void onOut(bool isErr, MemoryChunk mc) override {
            if (!isErr && output_) {
                output_->write(mc);
            }
        }

    private:
        File* output_;
    };

public:
    TsReadExPipe(AMTContext& ctx, const ConfigWrapper& setting)
        : output_(new File(setting.getTmpTsReadExDumpPath(), _T("wb")))
        , process_()
        , writeEnabled_(true) {
        ctx.info(_T("[tsreadex 解析]"));
        const tstring args = StringFormat(
            _T("\"%s\" -n -1 -r - -"), setting.getTsReadExPath().c_str());
        ctx.infoF(_T("tsreadex コマンド: %s"), args.c_str());
        process_.reset(new TraceProcess(args, output_.get()));
    }

    ~TsReadExPipe() {
        // EventBaseSubProcess::join()は2回呼ぶとclose済みパイプを操作してしまうため、
        // 明示的にjoin()済みの場合はここでは何もしない。
        if (process_ && !joined_) {
            try {
                process_->join();
            } catch (...) {
            }
        }
    }

    void write(MemoryChunk mc) {
        if (!writeEnabled_) {
            return;
        }
        try {
            process_->write(mc);
        } catch (const RuntimeException&) {
            // tsreadexが早期終了してもTS解析は最後まで継続する。
            writeEnabled_ = false;
        }
    }

    int join() {
        joined_ = true;
        return process_->join();
    }

private:
    std::unique_ptr<File> output_;
    std::unique_ptr<TraceProcess> process_;
    bool writeEnabled_;
    bool joined_ = false;
};

} // namespace

AMTSplitter::AMTSplitter(AMTContext& ctx, const ConfigWrapper& setting)
    : TsSplitter(ctx, true, true, isCaptionParsingEnabled(setting))
    , setting_(setting)
    , psWriter(ctx)
    , writeHandler(*this)
    , audioFile_(setting.getAudioFilePath(), _T("wb"))
    , waveFile_(setting.getWaveFilePath(), _T("wb"))
    , curVideoFormat_()
    , videoFileCount_(0)
    , videoStreamType_(-1)
    , audioStreamType_(-1)
    , audioFileSize_(0)
    , waveFileSize_(0)
    , srcFileSize_(0) {
    psWriter.setHandler(&writeHandler);
}

StreamReformInfo AMTSplitter::split() {
    readAll();

    // for debug
    printInteraceCount();

    return StreamReformInfo(ctx, videoFileCount_,
        videoFrameList_, audioFrameList_, captionTextList_, streamEventList_, timeList_);
}

int64_t AMTSplitter::getSrcFileSize() const {
    return srcFileSize_;
}

int64_t AMTSplitter::getTotalIntVideoSize() const {
    return writeHandler.getTotalSize();
}
AMTSplitter::StreamFileWriteHandler::StreamFileWriteHandler(TsSplitter& this_)
    : this_(this_), totalIntVideoSize_() {}
/* virtual */ void AMTSplitter::StreamFileWriteHandler::onStreamData(MemoryChunk mc) {
    if (file_ != NULL) {
        file_->write(mc);
        totalIntVideoSize_ += mc.length;
    }
}
void AMTSplitter::StreamFileWriteHandler::open(const tstring& path) {
    totalIntVideoSize_ = 0;
    file_ = std::unique_ptr<File>(new File(path, _T("wb")));
}
void AMTSplitter::StreamFileWriteHandler::close() {
    file_ = nullptr;
}
int64_t AMTSplitter::StreamFileWriteHandler::getTotalSize() const {
    return totalIntVideoSize_;
}

void AMTSplitter::readAll() {
    enum {
        BUFSIZE = 4 * 1024 * 1024,
        BUFFER_COUNT = 4
    };
    ReadAheadFile srcfile(setting_.getSrcFilePath(), BUFSIZE, BUFFER_COUNT);
    // tsreplaceで一時TSを使う場合だけ、入力TSのコピーを作成する。
    const bool needCopyTS = setting_.getFormat() == FORMAT_TSREPLACE
        && setting_.isMuxTsTempEnabled();
    std::unique_ptr<File> rawts;
    if (needCopyTS) {
        rawts.reset(new File(setting_.getTmpRawTSPath(), _T("wb")));
    }
    std::unique_ptr<TsReadExPipe> tsreadex;
    if (isTsReadExAvailable(setting_)) {
        tsreadex.reset(new TsReadExPipe(ctx, setting_));
    }
    srcFileSize_ = srcfile.size();
    while (true) {
        const MemoryChunk chunk = srcfile.read();
        if (chunk.length == 0) break;
        if (rawts) {
            rawts->write(chunk);
        }
        if (tsreadex) {
            tsreadex->write(chunk);
        }
        inputTsData(chunk);
    }
    if (tsreadex) {
        const int exitCode = tsreadex->join();
        if (exitCode != 0) {
            THROWF(FormatException, "tsreadexがエラーコード(%d)を返しました", exitCode);
        }
    }
}

/* static */ bool AMTSplitter::CheckPullDown(PICTURE_TYPE p0, PICTURE_TYPE p1) {
    switch (p0) {
    case PIC_TFF:
    case PIC_BFF_RFF:
        return (p1 == PIC_TFF || p1 == PIC_TFF_RFF);
    case PIC_BFF:
    case PIC_TFF_RFF:
        return (p1 == PIC_BFF || p1 == PIC_BFF_RFF);
    default: // それ以外はチェック対象外
        return true;
    }
}

void AMTSplitter::printInteraceCount() {

    if (videoFrameList_.size() == 0) {
        ctx.error(_T("フレームがありません"));
        return;
    }

    // ラップアラウンドしないPTSを生成
    std::vector<std::pair<int64_t, int>> modifiedPTS;
    int64_t videoBasePTS = videoFrameList_[0].PTS;
    int64_t prevPTS = videoFrameList_[0].PTS;
    for (int i = 0; i < int(videoFrameList_.size()); i++) {
        int64_t PTS = videoFrameList_[i].PTS;
        int64_t modPTS = prevPTS + int64_t((int32_t(PTS) - int32_t(prevPTS)));
        modifiedPTS.emplace_back(modPTS, i);
        prevPTS = modPTS;
    }

    // PTSでソート
    std::sort(modifiedPTS.begin(), modifiedPTS.end());

#if 0
    // フレームリストを出力
    FILE* framesfp = fopen("frames.txt", "w");
    fprintf(framesfp, "FrameNumber,DecodeFrameNumber,PTS,Duration,FRAME_TYPE,PIC_TYPE,IsGOPStart\n");
    for (int i = 0; i < (int)modifiedPTS.size(); i++) {
        int64_t PTS = modifiedPTS[i].first;
        int decodeIndex = modifiedPTS[i].second;
        const VideoFrameInfo& frame = videoFrameList_[decodeIndex];
        int PTSdiff = -1;
        if (i < (int)modifiedPTS.size() - 1) {
            int64_t nextPTS = modifiedPTS[i + 1].first;
            const VideoFrameInfo& nextFrame = videoFrameList_[modifiedPTS[i + 1].second];
            PTSdiff = int(nextPTS - PTS);
            if (CheckPullDown(frame.pic, nextFrame.pic) == false) {
                ctx.warnF(_T("Flag Check Error: PTS=%lld %s -> %s"),
                    PTS, PictureTypeString(frame.pic), PictureTypeString(nextFrame.pic));
            }
        }
        fprintf(framesfp, "%d,%d,%lld,%d,%s,%s,%d\n",
            i, decodeIndex, PTS, PTSdiff, FrameTypeString(frame.type), PictureTypeString(frame.pic), frame.isGopStart ? 1 : 0);
    }
    fclose(framesfp);
#endif

    // PTS間隔を出力
    struct Integer {
        int v;
        Integer() : v(0) {}
    };

    std::array<int, MAX_PIC_TYPE> interaceCounter = { 0 };
    std::map<int, Integer> PTSdiffMap;
    prevPTS = -1;
    for (const auto& ptsIndex : modifiedPTS) {
        int64_t PTS = ptsIndex.first;
        const VideoFrameInfo& frame = videoFrameList_[ptsIndex.second];
        interaceCounter[(int)frame.pic]++;
        if (prevPTS != -1) {
            int PTSdiff = int(PTS - prevPTS);
            PTSdiffMap[PTSdiff].v++;
        }
        prevPTS = PTS;
    }

    ctx.info(_T("[映像フレーム統計情報]"));

    int64_t totalTime = modifiedPTS.back().first - videoBasePTS;
    double sec = (double)totalTime / MPEG_CLOCK_HZ;
    int minutes = (int)(sec / 60);
    sec -= minutes * 60;
    ctx.infoF(_T("時間: %d分%.3f秒"), minutes, sec);

    ctx.infoF(_T("FRAME=%d DBL=%d TLP=%d TFF=%d BFF=%d TFF_RFF=%d BFF_RFF=%d"),
        interaceCounter[0], interaceCounter[1], interaceCounter[2], interaceCounter[3], interaceCounter[4], interaceCounter[5], interaceCounter[6]);

    for (const auto& pair : PTSdiffMap) {
        ctx.infoF(_T("(PTS_Diff,Cnt)=(%d,%d)"), pair.first, pair.second.v);
    }
}

// TsSplitter仮想関数 //

/* virtual */ void AMTSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    for (const VideoFrameInfo& frame : frames) {
        videoFrameList_.push_back(frame);
        videoFrameList_.back().fileOffset = writeHandler.getTotalSize();
    }
    psWriter.outVideoPesPacket(clock, frames, packet);
}

/* virtual */ void AMTSplitter::onVideoFormatChanged(VideoFormat fmt) {
    ctx.info(_T("[映像フォーマット変更]"));

    StringBuilder sb;
    sb.append("サイズ: %dx%d", fmt.width, fmt.height);
    if (fmt.width != fmt.displayWidth || fmt.height != fmt.displayHeight) {
        sb.append(" 表示領域: %dx%d", fmt.displayWidth, fmt.displayHeight);
    }
    int darW, darH; fmt.getDAR(darW, darH);
    sb.append(" (%d:%d)", darW, darH);
    if (fmt.fixedFrameRate) {
        sb.append(" FPS: %d/%d", fmt.frameRateNum, fmt.frameRateDenom);
    } else {
        sb.append(" FPS: VFR");
    }
    ctx.info(char_to_tstring(sb.str()));

    // ファイル変更
    if (!curVideoFormat_.isBasicEquals(fmt)) {
        // アスペクト比以外も変更されていたらファイルを分ける
        //（StreamReformと条件を合わせなければならないことに注意）
        writeHandler.open(setting_.getIntVideoFilePath(videoFileCount_++));
        psWriter.outHeader(videoStreamType_, audioStreamType_);
    }
    curVideoFormat_ = fmt;

    StreamEvent ev = StreamEvent();
    ev.type = VIDEO_FORMAT_CHANGED;
    ev.frameIdx = (int)videoFrameList_.size();
    streamEventList_.push_back(ev);
}

/* virtual */ void AMTSplitter::onAudioPesPacket(
    int audioIdx,
    int64_t clock,
    const std::vector<AudioFrameData>& frames,
    PESPacket packet) {
    for (const AudioFrameData& frame : frames) {
        FileAudioFrameInfo info = frame;
        info.audioIdx = audioIdx;
        info.codedDataSize = frame.codedDataSize;
        info.waveDataSize = frame.decodedDataSize;
        info.fileOffset = audioFileSize_;
        info.waveOffset = waveFileSize_;
        audioFile_.write(MemoryChunk(frame.codedData, frame.codedDataSize));
        if (frame.decodedDataSize > 0) {
            waveFile_.write(MemoryChunk((uint8_t*)frame.decodedData, frame.decodedDataSize));
        }
        audioFileSize_ += frame.codedDataSize;
        waveFileSize_ += frame.decodedDataSize;
        audioFrameList_.push_back(info);
    }
    if (videoFileCount_ > 0) {
        psWriter.outAudioPesPacket(audioIdx, clock, frames, packet);
    }
}

/* virtual */ void AMTSplitter::onAudioFormatChanged(int audioIdx, AudioFormat fmt) {
    ctx.infoF(_T("[音声%dフォーマット変更]"), audioIdx);
    ctx.infoF(_T("チャンネル: %s サンプルレート: %d"),
        getAudioChannelString(fmt.channels), fmt.sampleRate);

    StreamEvent ev = StreamEvent();
    ev.type = AUDIO_FORMAT_CHANGED;
    ev.audioIdx = audioIdx;
    ev.frameIdx = (int)audioFrameList_.size();
    streamEventList_.push_back(ev);
}

/* virtual */ void AMTSplitter::onCaptionPesPacket(
    int64_t clock,
    std::vector<CaptionItem>& captions,
    PESPacket packet) {
    for (auto& caption : captions) {
        captionTextList_.emplace_back(std::move(caption));
    }
}

/* virtual */ DRCSOutInfo AMTSplitter::getDRCSOutPath(int64_t PTS, const std::string& md5) {
    DRCSOutInfo info;
    info.elapsed = (videoFrameList_.size() > 0) ? (double)(PTS - videoFrameList_[0].PTS) : -1.0;
    info.filename = setting_.getDRCSOutPath(md5);
    return info;
}

// TsPacketSelectorHandler仮想関数 //

/* virtual */ void AMTSplitter::onPidTableChanged(const PMTESInfo video, const std::vector<PMTESInfo>& audio, const PMTESInfo caption) {
    // ベースクラスの処理
    TsSplitter::onPidTableChanged(video, audio, caption);

    ASSERT(audio.size() > 0);
    videoStreamType_ = video.stype;
    audioStreamType_ = audio[0].stype;

    StreamEvent ev = StreamEvent();
    ev.type = PID_TABLE_CHANGED;
    ev.numAudio = (int)audio.size();
    ev.frameIdx = (int)videoFrameList_.size();
    streamEventList_.push_back(ev);
}

/* virtual */ void AMTSplitter::onTime(int64_t clock, JSTTime time) {
    timeList_.push_back(std::make_pair(clock, time));
}


/* static */ tstring replaceOptions(
    const tstring& options,
    const VideoFormat& fmt,
    const ConfigWrapper& setting,
    const EncodeFileKey key,
    const int serviceID) {
    tstring ret = options;
    ret = str_replace(ret, _T("@IMAGE_WIDTH@"), StringFormat(_T("%d"), fmt.width));
    ret = str_replace(ret, _T("@IMAGE_HEIGHT@"), StringFormat(_T("%d"), fmt.height));
    ret = str_replace(ret, _T("@SERVICE_ID@"), StringFormat(_T("%d"), serviceID));
    ret = str_replace(ret, _T("@AMT_ENCODER@"), encoderToString(setting.getEncoder()));
    ret = str_replace(ret, _T("@AMT_AUDIO_ENCODER@"), audioEncoderToString(setting.getAudioEncoder()));
    ret = str_replace(ret, _T("@AMT_TEMP_DIR@"), setting.getTmpDir());
    ret = str_replace(ret, _T("@AMT_TEMP_VIDEO@"), _T("\"") + setting.getEncVideoFilePath(key) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_AUDIO@"), _T("\"") + setting.getIntAudioFilePath(key, 0, setting.getAudioEncoder()) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_AUDIO_0@"), _T("\"") + setting.getIntAudioFilePath(key, 0, setting.getAudioEncoder()) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_AUDIO_1@"), _T("\"") + setting.getIntAudioFilePath(key, 1, setting.getAudioEncoder()) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_CHAPTER@"), _T("\"") + setting.getTmpChapterPath(key) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_TIMECODE@"), _T("\"") + setting.getAvsTimecodePath(key) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS@"), _T("\"") + setting.getTmpASSFilePath(key, 0) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_0@"), _T("\"") + setting.getTmpASSFilePath(key, 0) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_1@"), _T("\"") + setting.getTmpASSFilePath(key, 1) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_SRT@"), _T("\"") + setting.getTmpSRTFilePath(key, 0) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_SRT_0@"), _T("\"") + setting.getTmpSRTFilePath(key, 0) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_SRT_1@"), _T("\"") + setting.getTmpSRTFilePath(key, 1) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_NICOJK_720S@"), _T("\"") + setting.getTmpNicoJKASSPath(key, NICOJK_720S) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_NICOJK_720T@"), _T("\"") + setting.getTmpNicoJKASSPath(key, NICOJK_720T) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_NICOJK_1080S@"), _T("\"") + setting.getTmpNicoJKASSPath(key, NICOJK_1080S) + _T("\""));
    ret = str_replace(ret, _T("@AMT_TEMP_ASS_NICOJK_1080T@"), _T("\"") + setting.getTmpNicoJKASSPath(key, NICOJK_1080T) + _T("\""));
    return ret;
}

void AddEnvironmentVariable(std::wstring& envBlock, const std::wstring& name, const std::wstring& value) {
    envBlock += name;
    envBlock += L"=";
    envBlock += value;
    envBlock += L'\0';
}

tstring GetDirectoryName(const tstring& path) {
    size_t lastSeparator = path.find_last_of(_T("/\\"));
    if (lastSeparator == tstring::npos) {
        return _T("");  // ディレクトリ区切りが見つからない場合は空文字列を返す
    }
    return path.substr(0, lastSeparator);
}

/* static */ int executeBatchFile(
    const tstring& batchPath,
    const VideoFormat& fmt,
    const ConfigWrapper& setting,
    const EncodeFileKey key,
    const int serviceID) {
    if (batchPath.empty()) {
        return 0;
    }
    
    // 一時バッチファイルのパスを生成
    tstring tempBatchPath = setting.getEncVideoFilePath(key) + _T(".") + rgy_get_extension(batchPath);
    
    try {
        // バッチファイルをコピー
        if (rgy_file_copy(batchPath.c_str(), tempBatchPath.c_str(), true) == 0) {
            return -1;
        }

        const auto outPath = StringFormat(_T("%s.%s"), setting.getOutFileBaseWithoutPrefix().c_str(), setting.getOutputExtention(setting.getFormat()));

        SetTemporaryEnvironmentVariable tmpvar;
        tmpvar.set(_T("CLI_IN_PATH"), setting.getSrcFilePath().c_str());
        tmpvar.set(_T("TS_IN_PATH"), setting.getSrcFileOriginalPath().c_str());
        tmpvar.set(_T("SERVICE_ID"), StringFormat(_T("%d"), serviceID).c_str());
        tmpvar.set(_T("CLI_OUT_PATH"), outPath);
        tmpvar.set(_T("TS_IN_DIR"), GetDirectoryName(setting.getSrcFileOriginalPath().c_str()));
        tmpvar.set(_T("CLI_OUT_DIR"), GetDirectoryName(outPath));
        tmpvar.set(_T("OUT_DIR"), GetDirectoryName(outPath));
        tmpvar.set(_T("IMAGE_WIDTH"), StringFormat(_T("%d"), fmt.width));
        tmpvar.set(_T("IMAGE_HEIGHT"), StringFormat(_T("%d"), fmt.height));
        tmpvar.set(_T("SERVICE_ID"), StringFormat(_T("%d"), serviceID));
        tmpvar.set(_T("AMT_ENCODER"), encoderToString(setting.getEncoder()));
        tmpvar.set(_T("AMT_AUDIO_ENCODER"), audioEncoderToString(setting.getAudioEncoder()));
        tmpvar.set(_T("AMT_TEMP_DIR"), setting.getTmpDir());
        tmpvar.set(_T("AMT_TEMP_AVS"), setting.getAvsTmpPath(key));
        tmpvar.set(_T("AMT_TEMP_AVS_TC"), setting.getAvsTimecodePath(key));
        tmpvar.set(_T("AMT_TEMP_AVS_DURATION"), setting.getAvsDurationPath(key));
        tmpvar.set(_T("AMT_TEMP_AFS_TC"), setting.getAfsTimecodePath(key));
        tmpvar.set(_T("AMT_TEMP_VIDEO"), setting.getEncVideoFilePath(key));
        tmpvar.set(_T("AMT_TEMP_AUDIO"), setting.getIntAudioFilePath(key, 0, setting.getAudioEncoder()));
        tmpvar.set(_T("AMT_TEMP_AUDIO_0"), setting.getIntAudioFilePath(key, 0, setting.getAudioEncoder()));
        tmpvar.set(_T("AMT_TEMP_AUDIO_1"), setting.getIntAudioFilePath(key, 1, setting.getAudioEncoder()));
        tmpvar.set(_T("AMT_TEMP_CHAPTER"), setting.getTmpChapterPath(key));
        tmpvar.set(_T("AMT_TEMP_TIMECODE"), setting.getAvsTimecodePath(key));
        tmpvar.set(_T("AMT_TEMP_ASS"), setting.getTmpASSFilePath(key, 0));
        tmpvar.set(_T("AMT_TEMP_ASS_0"), setting.getTmpASSFilePath(key, 0));
        tmpvar.set(_T("AMT_TEMP_ASS_1"), setting.getTmpASSFilePath(key, 1));
        tmpvar.set(_T("AMT_TEMP_SRT"), setting.getTmpSRTFilePath(key, 0));
        tmpvar.set(_T("AMT_TEMP_SRT_0"), setting.getTmpSRTFilePath(key, 0));
        tmpvar.set(_T("AMT_TEMP_SRT_1"), setting.getTmpSRTFilePath(key, 1));
        tmpvar.set(_T("AMT_TEMP_ASS_NICOJK_720S"), setting.getTmpNicoJKASSPath(key, NICOJK_720S));
        tmpvar.set(_T("AMT_TEMP_ASS_NICOJK_720T"), setting.getTmpNicoJKASSPath(key, NICOJK_720T));
        tmpvar.set(_T("AMT_TEMP_ASS_NICOJK_1080S"), setting.getTmpNicoJKASSPath(key, NICOJK_1080S));
        tmpvar.set(_T("AMT_TEMP_ASS_NICOJK_1080T"), setting.getTmpNicoJKASSPath(key, NICOJK_1080T));

        // バッチファイルを実行
        std::unique_ptr<RGYPipeProcess> process = createRGYPipeProcess();
        // 標準入出力は不要なので全て無効化
        process->init(PIPE_MODE_DISABLE, PIPE_MODE_DISABLE, PIPE_MODE_DISABLE);
        std::vector<tstring> args = { tempBatchPath };
        int runResult = process->run(args, setting.getTmpDir().c_str(), 0, false, true, true); // 最小化で実行
        if (runResult != 0) {
            return -1;
        }
        // プロセスの終了を待機し、終了コードを取得
        int exitCode = process->waitAndGetExitCode();
        return exitCode;
        
    } catch (...) {
        return -1;
    }
}

EncoderArgumentGenerator::EncoderArgumentGenerator(
    const ConfigWrapper& setting,
    StreamReformInfo& reformInfo)
    : setting_(setting)
    , reformInfo_(reformInfo) {}

tstring EncoderArgumentGenerator::GenEncoderOptions(
    int numFrames,
    VideoFormat outfmt,
    std::vector<BitrateZone> zones,
    double vfrBitrateScale,
    tstring timecodepath,
    int vfrTimingFps,
    EncodeFileKey key, int pass, int serviceID,
    const EncoderOptionInfo& eoInfo,
    const tstring& outPathOverride) {
    VIDEO_STREAM_FORMAT srcFormat = reformInfo_.getVideoStreamFormat();
    double srcBitrate = getSourceBitrate(key.video);
    const tstring outPath = (outPathOverride.size() > 0)
        ? outPathOverride
        : setting_.getEncVideoFilePath(key);
    return makeEncoderArgs(
        setting_.getEncoder(),
        setting_.getEncoderPath(),
        replaceOptions(setting_.getOptions(
            numFrames,
            srcFormat, srcBitrate, false, pass, zones, setting_.getEncVideoOptionFilePath(key), vfrBitrateScale, key, eoInfo),
            outfmt, setting_, key, serviceID),
        outfmt,
        timecodepath,
        vfrTimingFps,
        setting_.getFormat(),
        outPath);
}

// src, target
std::pair<double, double> EncoderArgumentGenerator::printBitrate(AMTContext& ctx, EncodeFileKey key) const {
    double srcBitrate = getSourceBitrate(key.video);
    ctx.infoF(_T("入力映像ビットレート: %d kbps"), (int)srcBitrate);
    VIDEO_STREAM_FORMAT srcFormat = reformInfo_.getVideoStreamFormat();
    double targetBitrate = std::numeric_limits<float>::quiet_NaN();
    if (setting_.isAutoBitrate()) {
        targetBitrate = setting_.getBitrate().getTargetBitrate(srcFormat, srcBitrate);
        if (key.cm == CMTYPE_CM) {
            targetBitrate *= setting_.getBitrateCM();
        }
        ctx.infoF(_T("目標映像ビットレート: %d kbps"), (int)targetBitrate);
    }
    return std::make_pair(srcBitrate, targetBitrate);
}

double EncoderArgumentGenerator::getSourceBitrate(int fileId) const {
    // ビットレート計算
    const auto& info = reformInfo_.getSrcVideoInfo(fileId);
    return ((double)info.first * 8 / 1000) / ((double)info.second / MPEG_CLOCK_HZ);
}

/* static */ std::vector<BitrateZone> MakeBitrateZones(
    const std::vector<double>& timeCodes,
    const std::vector<EncoderZone>& cmzones,
    const ConfigWrapper& setting,
    const EncoderOptionInfo& eoInfo,
    VideoInfo outvi) {
    std::vector<BitrateZone> bitrateZones;
    if (timeCodes.size() == 0 || setting.isEncoderSupportVFR()) {
        // VFRでない、または、エンコーダがVFRをサポートしている -> VFR用に調整する必要がない
        for (int i = 0; i < (int)cmzones.size(); i++) {
            bitrateZones.emplace_back(cmzones[i], setting.getBitrateCM(), setting.getCMQualityOffset());
        }
    } else {
        if (setting.isZoneAvailable()) {
            // VFR非対応エンコーダでゾーンに対応していればビットレートゾーン生成
#if 0
            {
                File dump("zone_param.dat", "wb");
                dump.writeArray(frameDurations);
                dump.writeArray(cmzones);
                dump.writeValue(setting.getBitrateCM());
                dump.writeValue(outvi.fps_numerator);
                dump.writeValue(outvi.fps_denominator);
                dump.writeValue(setting.getX265TimeFactor());
                dump.writeValue(0.05);
            }
#endif
            if (auto rcMode = getRCMode(setting.getEncoder(), eoInfo.rcMode);
                setting.isZoneWithQualityAvailable()   // 品質オフセットを--dynamic-rcで指定可能なエンコーダである
                && !setting.isAutoBitrate()            // 自動ビットレートでない
                && rcMode && !rcMode->isBitrateMode    // ビットレートモードでない
                && setting.getCMQualityOffset() != 0.0 // 品質オフセットが有効
            ) {
                for (int i = 0; i < (int)cmzones.size(); i++) {
                    bitrateZones.emplace_back(cmzones[i], setting.getBitrateCM(), setting.getCMQualityOffset());
                }
            } else {
                return MakeVFRBitrateZones(
                    timeCodes, cmzones, setting.getBitrateCM(),
                    outvi.fps_numerator, outvi.fps_denominator,
                    setting.getX265TimeFactor(), 0.05); // 全体で5%までの差なら許容する
            }
        }
    }
    return bitrateZones;
}

// ページヒープが機能しているかテスト
#if 0
void DoBadThing() {
    char *p = (char*)HeapAlloc(
        GetProcessHeap(),
        HEAP_GENERATE_EXCEPTIONS | HEAP_ZERO_MEMORY,
        8);
    memset(p, 'x', 32);
}
#endif

/* static */ void transcodeMain(AMTContext& ctx, const ConfigWrapper& setting) {
#if 0
    MessageBox(NULL, "Debug", "Amatsukaze", MB_OK);
    //DoBadThing();
#endif

    auto thSetPowerThrottling = std::make_unique<RGYThreadSetPowerThrottoling>(GetCurrentProcessId());
    thSetPowerThrottling->run(RGYThreadPowerThrottlingMode::Disabled);

    const_cast<ConfigWrapper&>(setting).CreateTempDir();
    setting.dump();

    bool isNoEncode = (setting.getMode() == _T("cm"));

    auto eoInfo = ParseEncoderOption(setting.getEncoder(), setting.getEncoderOptions());
    PrintEncoderInfo(ctx, eoInfo);
    const int cliParallel = setting.getEncoderParallel();
    const int encoderParallel = (cliParallel > 1) ? cliParallel : ((eoInfo.parallel > 1) ? eoInfo.parallel : 1);
    if (setting.isTwoPass() && encoderParallel > 1) {
        THROW(ArgumentException, "2passエンコード時は分割エンコードを使用できません (--enc-parallel / --parallel は無効です)");
    }

    // チェック
    if (!isNoEncode && !setting.isFormatVFRSupported() && eoInfo.afsTimecode) {
        THROW(FormatException, "M2TS/TS出力はVFRをサポートしていません");
    }
    if (setting.getFormat() == FORMAT_TSREPLACE) {
        auto cmtypes = setting.getCMTypes();
        if (cmtypes.size() != 1 || (cmtypes[0] != CMTYPE_BOTH && cmtypes[0] != CMTYPE_EDGE_TRIM)) {
            THROW(FormatException, "tsreplaceは前後CMカット以外のCMカットに対応していません");
        }
        if (eoInfo.format != VS_H264 && eoInfo.format != VS_H265) {
            THROW(FormatException, "tsreplaceはH.264/H.265以外には対応していません");
        }
    }

    ResourceManger rm(ctx, setting.getInPipe(), setting.getOutPipe());
    rm.wait(HOST_CMD_TSAnalyze);

    Stopwatch sw;
    sw.start();
    ResumeInfo resumeInfo;
    std::unique_ptr<StreamReformInfo> reformInfoPtr;
    const bool isReusingTmp = !isNoEncode && tryLoadResume(ctx, setting, resumeInfo, reformInfoPtr);
    const bool captionsParsed = isReusingTmp ? resumeInfo.captionsParsed : isCaptionParsingEnabled(setting);
    int serviceId;
    int64_t numTotalPackets;
    int64_t numScramblePackets;
    int64_t totalIntVideoSize;
    int64_t srcFileSize;
    int noDrcsMapCount;
    if (isReusingTmp) {
        serviceId = resumeInfo.serviceId;
        numTotalPackets = resumeInfo.numTotalPackets;
        numScramblePackets = resumeInfo.numScramblePackets;
        totalIntVideoSize = resumeInfo.totalIntVideoSize;
        srcFileSize = resumeInfo.splitterSrcFileSize;
        noDrcsMapCount = resumeInfo.noDrcsMapCount;
    } else {
        auto splitter = std::unique_ptr<AMTSplitter>(new AMTSplitter(ctx, setting));
        if (setting.getServiceId() > 0) {
            splitter->setServiceId(setting.getServiceId());
        }
        reformInfoPtr = std::make_unique<StreamReformInfo>(splitter->split());
        ctx.infoF(_T("TS解析完了: %.2f秒"), sw.getAndReset());
        if (captionsParsed && !setting.isSubtitlesEnabled()) {
            ctx.info(_T("[一時ファイル再利用] 再開情報保存用に字幕を解析しました（字幕処理は無効のため出力しません）"));
        }
        serviceId = splitter->getActualServiceId();
        numTotalPackets = splitter->getNumTotalPackets();
        numScramblePackets = splitter->getNumScramblePackets();
        totalIntVideoSize = splitter->getTotalIntVideoSize();
        srcFileSize = splitter->getSrcFileSize();
        noDrcsMapCount = ctx.getErrorCount(AMT_ERR_NO_DRCS_MAP);
    }
    StreamReformInfo& reformInfo = *reformInfoPtr;

    if (!isReusingTmp && setting.isNoRemoveTmp()) {
        saveResumeStreamInfo(ctx, setting, reformInfo);
    }
    if (!isReusingTmp && captionsParsed && !setting.isSubtitlesEnabled()) {
        reformInfo.clearCaptionItems();
    }

    if (setting.isDumpStreamInfo()) {
        reformInfo.serialize(setting.getStreamInfoPath());
    }

    // スクランブルパケットチェック
    double scrambleRatio = (double)numScramblePackets / (double)numTotalPackets;
    if (scrambleRatio > 0.01) {
        ctx.errorF(_T("%.2f%%のパケットがスクランブル状態です。"), scrambleRatio * 100);
        if (scrambleRatio > 0.3) {
            THROW(FormatException, "スクランブルパケットが多すぎます");
        }
    }

    if (!isNoEncode && setting.isIgnoreNoDrcsMap() == false) {
        // DRCSマッピングチェック
        if ((setting.isSubtitlesEnabled() ? noDrcsMapCount : 0) > 0) {
            THROW(NoDrcsMapException, "マッピングにないDRCS外字あり正常に字幕処理できなかったため終了します");
        }
    }

    if (!isReusingTmp) {
        reformInfo.prepare(setting.isSplitSub(), setting.isEncodeAudio(), setting.getFormat() == FORMAT_TSREPLACE);
    }

    time_t startTime = reformInfo.getFirstFrameTime();

    NicoJK nicoJK(ctx, setting);
    bool nicoOK = false;
    if (!isNoEncode && setting.isNicoJKEnabled()) {
        ctx.info(_T("[ニコニコ実況コメント取得]"));
        auto srcDuration = reformInfo.getInDuration() / MPEG_CLOCK_HZ;
        nicoOK = nicoJK.makeASS(serviceId, startTime, (int)srcDuration);
        if (nicoOK) {
            reformInfo.SetNicoJKList(nicoJK.getDialogues());
        } else {
            if (nicoJK.isFail() == false) {
                ctx.info(_T("対応チャンネルがありません"));
            } else if (setting.isIgnoreNicoJKError() == false) {
                THROW(RuntimeException, "ニコニコ実況コメント取得に失敗");
            }
        }
    }

    int numVideoFiles = reformInfo.getNumVideoFile();
    int mainFileIndex = reformInfo.getMainVideoFileIndex();
    std::vector<std::unique_ptr<CMAnalyze>> cmanalyze;

    // ソースファイル読み込み用データ保存
    if (isReusingTmp) {
        ctx.info(_T("[一時ファイル再利用] 現在の設定でソースファイル読み込み用データを再生成します"));
    }
    for (int videoFileIndex = 0; videoFileIndex < numVideoFiles; videoFileIndex++) {
        // ファイル読み込み情報を保存
        auto& fmt = reformInfo.getFormat(EncodeFileKey(videoFileIndex, 0));
        auto amtsPath = setting.getTmpAMTSourcePath(videoFileIndex);
        ctx.infoF(_T("ソースファイル読み込み用データ保存[%d/%d]: %s"), videoFileIndex + 1, numVideoFiles, amtsPath.c_str());
        av::SaveAMTSource(amtsPath,
            setting.getIntVideoFilePath(videoFileIndex),
            setting.getWaveFilePath(),
            fmt.videoFormat, fmt.audioFormat[0],
            reformInfo.getFilterSourceFrames(videoFileIndex),
            reformInfo.getFilterSourceAudioFrames(videoFileIndex),
            setting.getDecoderSetting());
        ctx.infoF(_T("ソースファイル読み込み用データ保存完了[%d/%d]"), videoFileIndex + 1, numVideoFiles);
    }

    // ロゴ・CM解析
    if (!isReusingTmp) {
        rm.wait(HOST_CMD_CMAnalyze);
        ctx.infoF(_T("[ロゴ・CM解析]"));
        sw.start();
    } else {
        ctx.info(_T("[一時ファイル再利用] ロゴ・CM解析結果を再利用します"));
    }
    std::vector<std::pair<size_t, bool>> logoFound;
    std::vector<std::unique_ptr<MakeChapter>> chapterMakers(numVideoFiles);
    for (int videoFileIndex = 0; videoFileIndex < numVideoFiles; videoFileIndex++) {
        cmanalyze.push_back(std::make_unique<CMAnalyze>(ctx, setting));
        const int numFrames = (int)reformInfo.getFilterSourceFrames(videoFileIndex).size();
        const bool delogoEnabled = setting.isNoDelogo() ? false : true;
        // チャプター解析は300フレーム（約10秒）以上ある場合だけ
        //（短すぎるとエラーになることがあるので
        const bool analyzeChapterAndCM = (setting.isChapterEnabled() && numFrames >= 300);
        CMAnalyze *cma = cmanalyze.back().get();
        if (isReusingTmp) {
            const auto& resumeVideo = resumeInfo.videos[videoFileIndex];
            cma->restore(resumeVideo.logoPath, resumeVideo.trims, resumeVideo.divs, numFrames);
        } else {
            const auto& inputVideofmt = reformInfo.getFormat(EncodeFileKey(videoFileIndex, 0)).videoFormat;
            if (analyzeChapterAndCM || delogoEnabled) {
                cma->analyze(serviceId, videoFileIndex, inputVideofmt, numFrames, analyzeChapterAndCM);
            }

            if (analyzeChapterAndCM && setting.isPmtCutEnabled()) {
                // PMT変更によるCM追加認識
                cma->applyPmtCut(numFrames, setting.getPmtCutSideRate(),
                    reformInfo.getPidChangedList(videoFileIndex));
            }
        }

        if (videoFileIndex == mainFileIndex) {
            if (setting.getTrimAVSPath().size()) {
                // Trim情報入力
                cma->inputTrimAVS(numFrames, setting.getTrimAVSPath());
            }
        }

        logoFound.emplace_back(numFrames, cma->getLogoPath().size() > 0);
        reformInfo.applyCMZones(videoFileIndex, cma->getZones(), cma->getDivs());

        if (analyzeChapterAndCM) {
            chapterMakers[videoFileIndex] = std::unique_ptr<MakeChapter>(
                new MakeChapter(ctx, setting, reformInfo, videoFileIndex, cma->getTrims()));
        }
    }
    if (setting.isChapterEnabled()) {
        // ロゴがあったかチェック //
        // 映像ファイルをフレーム数でソート
        std::sort(logoFound.begin(), logoFound.end());
        if (setting.getLogoPath().size() > 0 && // ロゴ指定あり
            setting.isIgnoreNoLogo() == false &&          // ロゴなし無視でない
            logoFound.back().first >= 300 &&
            logoFound.back().second == false)     // 最も長い映像でロゴが見つからなかった
        {
            THROW(NoLogoException, "マッチするロゴが見つかりませんでした");
        }
        ctx.infoF(_T("ロゴ・CM解析完了: %.2f秒"), sw.getAndReset());
    }

    if (setting.isNoRemoveTmp()) {
        saveResumeFiles(ctx, setting, reformInfo, cmanalyze,
            serviceId, numTotalPackets, numScramblePackets, totalIntVideoSize, srcFileSize,
            noDrcsMapCount, captionsParsed);
    }

    if (isNoEncode) {
        if (setting.isCopyTrimAVSEnabled()) {
            copyTrimAVSForCMOnly(ctx, setting, reformInfo, numVideoFiles);
        }
        if (setting.isOutputChapterEnabled()) {
            ctx.info(_T("[チャプター生成]"));
            for (const auto& key : reformInfo.getOutFileKeys()) {
                const auto& fileIn = reformInfo.getEncodeFile(key);
                if (fileIn.duration >= MPEG_CLOCK_HZ /*1秒以下なら出力しない*/ && chapterMakers[key.video]) {
                    chapterMakers[key.video]->exec(key);
                    const auto path = setting.getTmpChapterPath(key);
                    if (File::exists(path)) {
                        const auto dstchapter = setting.getOutChapterPath(fileIn.key, fileIn.keyMax, setting.getFormat(), eoInfo.format);
                        File::copy(path, dstchapter);
                    }
                }
            }
        }
        return; // CM解析のみならここで終了
    }

    auto audioDiffInfo = reformInfo.genAudio(setting.getCMTypes());
    audioDiffInfo.printAudioPtsDiff(ctx);

    const auto& allKeys = reformInfo.getOutFileKeys();
    std::vector<EncodeFileKey> keys;
    // 1秒以下なら出力しない
    std::copy_if(allKeys.begin(), allKeys.end(), std::back_inserter(keys),
        [&](EncodeFileKey key) { return reformInfo.getEncodeFile(key).duration >= MPEG_CLOCK_HZ; });

    std::vector<EncodeFileOutput> outFileInfo(keys.size());

    ctx.info(_T("[チャプター生成]"));
    for (int i = 0; i < (int)keys.size(); i++) {
        auto key = keys[i];
        if (chapterMakers[key.video]) {
            chapterMakers[key.video]->exec(key);
        }
    }

    std::vector<WhisperAudioEntry> whisperAudioEntries;
    struct WhisperTask {
        int keyIndex;
        EncodeFileKey key;
        int localIndex;
        WhisperProcessParam param;
        tstring srtPath;
        tstring vttPath;
        std::unique_ptr<StdRedirectedSubProcess> process;
        int exitCode = -1; // join()の結果 (-1: 未実行)
    };
    std::vector<WhisperTask> whisperTasks;
    std::vector<int> whisperLocalIndex(keys.size(), 0);
    if (setting.isEncodeAudio()) {
        ctx.info(_T("[音声エンコード]"));
        for (int i = 0; i < (int)keys.size(); i++) {
            auto key = keys[i];
            auto outpath = setting.getIntAudioFilePath(key, 0, setting.getAudioEncoder());
            auto args = makeAudioEncoderArgs(
                setting.getAudioEncoder(),
                setting.getAudioEncoderPath(),
                setting.getAudioEncoderOptions(),
                setting.getAudioBitrateInKbps(),
                outpath);
            auto format = reformInfo.getFormat(key);
            auto audioFrames = reformInfo.getWaveInput(reformInfo.getEncodeFile(key).audioFrames[0]);
            EncodeAudio(ctx, args, setting.getWaveFilePath(), format.audioFormat[0], audioFrames);
            whisperAudioEntries.push_back({ i, key, 0, outpath, 0, -1 });
        }
    } else if (setting.getFormat() != FORMAT_TSREPLACE
        || (setting.getSubtitleMode() == SUBMODE_WHISPER_ALWAYS || setting.getSubtitleMode() == SUBMODE_WHISPER_FALLBACK)) { // tsreplaceの場合は音声ファイルを作らない
        ctx.info(_T("[音声出力]"));
        PacketCache audioCache(ctx, setting.getAudioFilePath(), reformInfo.getAudioFileOffsets(), 12, 4);
        for (int i = 0; i < (int)keys.size(); i++) {
            const auto key = keys[i];
            const auto& fileIn = reformInfo.getEncodeFile(key);
            const auto fmt = reformInfo.getFormat(key);
            for (int asrc = 0, adst = 0; asrc < (int)fileIn.audioFrames.size(); asrc++) {
                const std::vector<int>& frameList = fileIn.audioFrames[asrc];
                if (frameList.size() > 0) {
                    const bool isDualMono = (fmt.audioFormat[asrc].channels == AUDIO_2LANG);
                    if (!setting.isEncodeAudio() && isDualMono) {
                        // デュアルモノは2つのAACに分離
                        ctx.infoF(_T("音声%d-%dはデュアルモノなので2つのAACファイルに分離します"), fileIn.outKey.format, asrc);
                        const int adst0 = adst++;
                        const int adst1 = adst++;
                        SpDualMonoSplitter splitter(ctx);
                        const tstring filepath0 = setting.getIntAudioFilePath(key, adst0, setting.getAudioEncoder());
                        whisperAudioEntries.push_back({ i, key, adst0, filepath0, asrc, 0 });
                        const tstring filepath1 = setting.getIntAudioFilePath(key, adst1, setting.getAudioEncoder());
                        whisperAudioEntries.push_back({ i, key, adst1, filepath1, asrc, 1 });
                        splitter.open(0, filepath0);
                        splitter.open(1, filepath1);
                        for (int frameIndex : frameList) {
                            splitter.inputPacket(audioCache[frameIndex]);
                        }
                        ctx.infoF(_T("音声%d-%d[0]出力 -> %s"), fileIn.outKey.format, asrc, filepath0.c_str());
                        ctx.infoF(_T("音声%d-%d[1]出力 -> %s"), fileIn.outKey.format, asrc, filepath1.c_str());
                    } else {
                        if (isDualMono) {
                            ctx.infoF(_T("音声%d-%dはデュアルモノですが、音声フォーマット無視指定があるので分離しません"), fileIn.outKey.format, asrc);
                        }
                        const int adst0 = adst++;
                        const tstring filepath = setting.getIntAudioFilePath(key, adst0, setting.getAudioEncoder());
                        whisperAudioEntries.push_back({ i, key, adst0, filepath, asrc, -1 });
                        File file(filepath, _T("wb"));
                        for (int frameIndex : frameList) {
                            file.write(audioCache[frameIndex]);
                        }
                        ctx.infoF(_T("音声%d-%d出力 -> %s"), fileIn.outKey.format, asrc, filepath.c_str());
                    }
                }
            }
        }
    }

    std::vector<PsisiarcTask> psisiarcTasks;
    ctx.info(_T("[字幕ファイル生成]"));
    for (int i = 0; i < (int)keys.size(); i++) {
        auto key = keys[i];
        CaptionASSFormatter formatterASS(ctx);
        CaptionSRTFormatter formatterSRT(ctx);
        NicoJKFormatter formatterNicoJK(ctx);
        const auto& capList = reformInfo.getEncodeFile(key).captionList;
        for (int lang = 0; lang < (int)capList.size(); lang++) {
            auto ass = formatterASS.generate(capList[lang]);
            auto srt = formatterSRT.generate(capList[lang]);
            WriteUTF8File(setting.getTmpASSFilePath(key, lang), ass);
            ctx.infoF(_T("字幕ファイル出力: %s"), setting.getTmpASSFilePath(key, lang).c_str());
            if (srt.size() > 0) {
                // SRTはCP_STR_SMALLしかなかった場合など出力がない場合があり、
                // 空ファイルはmux時にエラーになるので、1行もない場合は出力しない
                WriteUTF8File(setting.getTmpSRTFilePath(key, lang), srt);
                ctx.infoF(_T("字幕ファイル出力: %s"), setting.getTmpSRTFilePath(key, lang).c_str());
            }
        }
        if (nicoOK) {
            const auto& headerLines = nicoJK.getHeaderLines();
            const auto& dialogues = reformInfo.getEncodeFile(key).nicojkList;
            for (NicoJKType jktype : setting.getNicoJKTypes()) {
                File file(setting.getTmpNicoJKASSPath(key, jktype), _T("w"));
                auto text = formatterNicoJK.generate(headerLines[(int)jktype], dialogues[(int)jktype]);
                file.write(MemoryChunk((uint8_t*)text.data(), text.size()));
            }
        }
        // 字幕構築 + (必要なら) WebVTT生成
        try {
            if (setting.isWebVTTEnabled()) {
                reformInfo.genWebVTT(key, setting, psisiarcTasks);
            }
        } catch (const Exception& e) {
            ctx.warnF(_T("WebVTT生成に失敗: %s"), e.message());
        }

        // Whisperによる字幕生成 (モード制御 + 複数音声)
        try {
            if (setting.getSubtitleMode() == SUBMODE_WHISPER_ALWAYS || setting.getSubtitleMode() == SUBMODE_WHISPER_FALLBACK) {
                SubtitleGenerator whisperGen(ctx);
                const auto wdir    = setting.getTmpWhisperDir();
                const auto whisper = setting.getWhisperPath();
                // 追加オプション組み立て
                StringBuilderT opt;
                if (setting.getWhisperModel().size() > 0) opt.append(_T("--model %s"), setting.getWhisperModel());
                if (setting.getWhisperOption().size() > 0) {
                    if (opt.str().size() > 0) opt.append(_T(" "));
                    opt.append(_T("%s"), setting.getWhisperOption());
                }
                const tstring extraOpt = opt.str();
                const bool fallbackOnly = (setting.getSubtitleMode() == SUBMODE_WHISPER_FALLBACK);
                const bool haveArib = (reformInfo.getEncodeFile(key).captionList.size() > 0);
                const bool shouldRun = (!fallbackOnly) || (fallbackOnly && !haveArib);
                if (shouldRun) {
                    const bool isWhisperCpp = exeIsWhisperCpp(whisper);
                    for (const auto& entry : whisperAudioEntries) {
                        if (entry.keyIndex != i) {
                            continue;
                        }

                        tstring whisperInput = entry.audioPath;
                        if (isWhisperCpp && !_tcheck_ext(whisperInput.c_str(), _T(".wav"))) {
                            try {
                                tstring wavInput = createWhisperWaveInput(ctx, setting, reformInfo, entry);
                                if (!wavInput.empty()) {
                                    whisperInput = wavInput;
                                } else {
                                    ctx.warnF(_T("whisper-cpp用のwav生成に失敗したため、元の音声を使用します: %s"), whisperInput.c_str());
                                }
                            } catch (const Exception& e) {
                                ctx.warnF(_T("whisper-cpp用wav生成中に例外: %s"), e.message());
                                ctx.warnF(_T("元の音声を使用します: %s"), whisperInput.c_str());
                            }
                        }

                        if (whisperInput.empty()) {
                            ctx.warn(_T("Whisper字幕生成: 音声入力パスが空のためスキップします"));
                            continue;
                        }
                        const tstring outFileWithoutExt = setting.getTmpWhisperFilenameWithoutExt(entry.key, entry.localIndex);
                        const auto srtPath = setting.getTmpWhisperSrtPath(entry.key, entry.localIndex);
                        const auto vttPath = setting.getTmpWhisperVttPath(entry.key, entry.localIndex);

                        if (setting.isWhisperParallelEnabled()) {
                            // エンコードと並列実行: ここではタスクだけ登録し、別スレッドで直列実行する
                            WhisperTask task;
                            task.keyIndex = i;
                            task.key = entry.key;
                            task.localIndex = entry.localIndex;
                            task.param.whisperPath = whisper;
                            task.param.audioPath = whisperInput;
                            task.param.outDir = wdir;
                            task.param.outFileWithoutExt = outFileWithoutExt;
                            task.param.extraOptions = extraOpt;
                            task.param.enableVtt = setting.isWebVTTEnabled();
                            task.param.isUtf8Log = true;
                            task.param.captureOnly = true;
                            task.srtPath = srtPath;
                            task.vttPath = vttPath;
                            task.process = nullptr;
                            whisperTasks.push_back(std::move(task));
                        } else {
                            // 従来どおり同期実行（構造体で一括指定）
                            WhisperProcessParam param;
                            param.whisperPath       = whisper;
                            param.audioPath         = whisperInput;
                            param.outDir            = wdir;
                            param.outFileWithoutExt = outFileWithoutExt;
                            param.extraOptions      = extraOpt;
                            param.enableVtt         = setting.isWebVTTEnabled();
                            param.isUtf8Log         = true;
                            param.captureOnly       = false;
                            whisperGen.runWhisper(param);
                            // 空ファイルになっていたら削除する
                            uint64_t filesize = 0;
                            if (rgy_file_exists(srtPath) && rgy_get_filesize(srtPath.c_str(), &filesize) && filesize == 0) {
                                rgy_file_remove(srtPath.c_str());
                            }
                            if (rgy_file_exists(vttPath) && rgy_get_filesize(vttPath.c_str(), &filesize) && filesize == 0) {
                                rgy_file_remove(vttPath.c_str());
                            }
                        }
                    }
                }
            }
        } catch (const Exception& e) {
            ctx.warnF(_T("Whisper字幕生成に失敗: %s"), e.message());
        }
    }
    ctx.infoF(_T("字幕ファイル生成完了: %.2f秒"), sw.getAndReset());

    auto argGen = std::unique_ptr<EncoderArgumentGenerator>(new EncoderArgumentGenerator(setting, reformInfo));

    // Whisper並列実行時は、別スレッドでwhisperTasksを直列実行する
    std::unique_ptr<std::thread> whisperThread;
    if (setting.isWhisperParallelEnabled() && !whisperTasks.empty()) {
        whisperThread = std::make_unique<std::thread>([&ctx, &whisperTasks]() {
            SubtitleGenerator whisperGen(ctx);
            for (auto& task : whisperTasks) {
                task.process = whisperGen.startWhisperProcess(task.param);
                task.exitCode = task.process->join();
            }
        });
    }

    // psisiarcは専用スレッドでタスクを直列実行し、映像エンコードと並行させる。
    std::unique_ptr<std::thread> psisiarcThread;
    if (!psisiarcTasks.empty()) {
        psisiarcThread = std::make_unique<std::thread>([&psisiarcTasks]() {
            for (auto& task : psisiarcTasks) {
                // 例外がスレッド外へ漏れるとterminateするため、タスク単位で捕捉して継続する。
                // ログはスレッドセーフでないため出さず、同期時にまとめて報告する。
                try {
                    task.process = std::make_unique<StdRedirectedSubProcess>(
                        task.cmd, 0, false, false, true);
                    task.exitCode = task.process->join();
                } catch (const Exception& e) {
                    task.errorMessage = e.message();
                } catch (...) {
                    task.errorMessage = _T("不明なエラー");
                }
            }
        });
    }
    // エンコード中の例外でunwindする場合もjoinを保証し、joinable時のterminateを防ぐ。
    struct PsisiarcThreadJoiner {
        std::unique_ptr<std::thread>& thread;
        ~PsisiarcThreadJoiner() {
            if (thread && thread->joinable()) {
                thread->join();
            }
        }
    } psisiarcThreadJoiner{ psisiarcThread };

    sw.start();
    for (int i = 0; i < (int)keys.size(); i++) {
        auto key = keys[i];
        auto& fileOut = outFileInfo[i];
        const CMAnalyze* cma = cmanalyze[key.video].get();

        AMTFilterSource filterSource(ctx, setting, reformInfo,
            cma->getZones(), cma->getLogoPath(), key, rm);

        if (!setting.getPreEncBatchFile().empty()) {
            ctx.infoF(_T("[エンコード前バッチファイル] %d/%d"), i + 1, (int)keys.size());
            ctx.infoF(_T("%s"), setting.getPreEncBatchFile().c_str());
            std::unique_ptr<RGYMutex> mutexOpt;
            if (setting.isExclusiveBatExec()) {
                mutexOpt = std::make_unique<RGYMutex>("AmatsukazeCLIPreEncodeBatMutex");
            }
            {
                std::unique_ptr<RGYMutex::Guard> guard = (mutexOpt ? std::make_unique<RGYMutex::Guard>(*mutexOpt) : nullptr);
                if (executeBatchFile(setting.getPreEncBatchFile(), filterSource.getFormat(), setting, key, serviceId)) {
                    THROW(RuntimeException, "エンコード前バッチファイルの実行に失敗しました");
                }
            }
        }

        try {
            PClip filterClip = filterSource.getClip();
            IScriptEnvironment2* env = filterSource.getEnv();
            auto encoderZones = filterSource.getZones();
            auto& outfmt = filterSource.getFormat();
            auto& outvi = filterClip->GetVideoInfo();
            auto& timeCodes = filterSource.getTimeCodes();

            ctx.infoF(_T("[エンコード開始] %d/%d %s"), i + 1, (int)keys.size(), CMTypeToString(key.cm));
            auto bitrate = argGen->printBitrate(ctx, key);

            fileOut.vfmt = outfmt;
            fileOut.srcBitrate = bitrate.first;
            fileOut.targetBitrate = bitrate.second;
            fileOut.vfrTimingFps = filterSource.getVfrTimingFps();

            if (timeCodes.size() > 0) {
                // フィルタによるVFRが有効
                if (eoInfo.afsTimecode) {
                    THROW(ArgumentException, "エンコーダとフィルタの両方でVFRタイムコードが出力されています。");
                }
                if (eoInfo.selectEvery > 1) {
                    THROW(ArgumentException, "VFRで出力する場合は、エンコーダで間引くことはできません");
                } else if (!setting.isFormatVFRSupported()) {
                    THROW(FormatException, "M2TS/TS出力はVFRをサポートしていません");
                }
                ctx.infoF(_T("VFRタイミング: %d fps"), fileOut.vfrTimingFps);
                fileOut.timecode = setting.getAvsTimecodePath(key);
            }
            if (encoderParallel > 1) {
                ctx.infoF(_T("分割エンコード: %d"), encoderParallel);
            }

            std::vector<int> passList;
            if (setting.isTwoPass()) {
                passList.push_back(1);
                passList.push_back(2);
            } else {
                passList.push_back(-1);
            }

            auto bitrateZones = MakeBitrateZones(timeCodes, encoderZones, setting, eoInfo, outvi);
            auto vfrBitrateScale = AdjustVFRBitrate(timeCodes, outvi.fps_numerator, outvi.fps_denominator);
            const tstring baseTimecodePath = fileOut.timecode;
            const tstring baseOutputPath = setting.getEncVideoFilePath(key);
            // x264, x265, SVT-AV1のときはdisablePowerThrottoling=trueとする
            // QSV/NV/VCEEncではプロセス内で自動的に最適なように設定されるため不要
            const bool disablePowerThrottoling = (setting.getEncoder() == ENCODER_X264 || setting.getEncoder() == ENCODER_X265 || setting.getEncoder() == ENCODER_SVTAV1);

            AMTFilterVideoEncoder encoder(ctx, setting, std::max(4, setting.getNumEncodeBufferFrames()));
            // 並列GetFrame用にフィルタチェーンを構築するファクトリを渡す
            // 既存のfilterSourceの前処理結果（スクリプト）を再利用して環境を再構築
            auto filterFactory = [&]() -> std::unique_ptr<AMTFilterSource> {
                return std::unique_ptr<AMTFilterSource>(new AMTFilterSource(ctx, filterSource));
            };

            encoder.encode(filterClip, outfmt,
                timeCodes, *argGen, passList, bitrateZones, vfrBitrateScale,
                baseTimecodePath, fileOut.vfrTimingFps, baseOutputPath,
                key, serviceId, eoInfo, encoderParallel, disablePowerThrottoling,
                env, filterFactory, setting.getEncoder());
        } catch (const AvisynthError& avserror) {
            THROWF(AviSynthException, "%s", avserror.msg);
        }
    }
    ctx.infoF(_T("エンコード完了: %.2f秒"), sw.getAndReset());

    argGen = nullptr;

    // Whisper並列実行時はここでバックグラウンドスレッドの完了待ち＆ログ出力を行う
    if (setting.isWhisperParallelEnabled() && !whisperTasks.empty()) {
        if (whisperThread && whisperThread->joinable()) {
            ctx.info(_T("[Whisper字幕生成: バックグラウンド処理の完了待ち]"));
            whisperThread->join();
            whisperThread.reset();
        }
        for (auto& task : whisperTasks) {
            if (!task.process) {
                continue;
            }
            const int ret = task.exitCode;

            const auto& lines = task.process->getCapturedLines();
            if (!lines.empty()) {
                ctx.info(_T("↓↓↓↓↓↓Whisper出力↓↓↓↓↓↓"));
                for (const auto& v : lines) {
                    std::vector<char> buf = v;
                    if (buf.empty() || buf.back() != '\0') {
                        buf.push_back('\0');
                    }
                    ctx.infoF(_T("%s"), char_to_tstring(buf.data()));
                }
                ctx.info(_T("↑↑↑↑↑↑Whisper出力↑↑↑↑↑↑"));
            }

            if (ret != 0) {
                ctx.warnF(_T("Whisper字幕生成に失敗 (終了コード: 0x%x)"), ret);
                continue;
            }

            // 正常終了時のみ、空SRT/VTTファイルの削除を行う
            uint64_t filesize = 0;
            if (rgy_file_exists(task.srtPath) && rgy_get_filesize(task.srtPath.c_str(), &filesize) && filesize == 0) {
                rgy_file_remove(task.srtPath.c_str());
            }
            if (rgy_file_exists(task.vttPath) && rgy_get_filesize(task.vttPath.c_str(), &filesize) && filesize == 0) {
                rgy_file_remove(task.vttPath.c_str());
            }
        }
    }

    // muxがpscを読む前にpsisiarcの完了を待ち、捕捉した出力をまとめて記録する。
    if (!psisiarcTasks.empty()) {
        if (psisiarcThread && psisiarcThread->joinable()) {
            ctx.info(_T("[psisiarc: バックグラウンド処理の完了待ち]"));
            psisiarcThread->join();
            psisiarcThread.reset();
        }
        for (auto& task : psisiarcTasks) {
            if (!task.errorMessage.empty()) {
                ctx.warnF(_T("psisiarcの実行に失敗: %s"), task.errorMessage.c_str());
                continue;
            }
            if (!task.process) {
                continue;
            }
            const auto& lines = task.process->getCapturedLines();
            if (!lines.empty()) {
                ctx.info(_T("↓↓↓↓↓↓psisiarc出力↓↓↓↓↓↓"));
                for (const auto& line : lines) {
                    std::vector<char> buffer = line;
                    if (buffer.empty() || buffer.back() != '\0') {
                        buffer.push_back('\0');
                    }
                    ctx.infoF(_T("%s"), char_to_tstring(buffer.data()));
                }
                ctx.info(_T("↑↑↑↑↑↑psisiarc出力↑↑↑↑↑↑"));
            }
            if (task.exitCode != 0) {
                ctx.warnF(_T("psisiarcがエラーコード(%d)を返しました"), task.exitCode);
            }
        }
    }

    rm.wait(HOST_CMD_Mux);
    sw.start();
    int64_t totalOutSize = 0;
    auto muxer = std::unique_ptr<AMTMuxder>(new AMTMuxder(ctx, setting, reformInfo));
    for (int i = 0; i < (int)keys.size(); i++) {
        auto key = keys[i];

        ctx.infoF(_T("[Mux開始] %d/%d %s"), i + 1, (int)keys.size(), CMTypeToString(key.cm));
        muxer->mux(key, eoInfo, nicoOK, outFileInfo[i]);

        totalOutSize += outFileInfo[i].fileSize;
    }
    ctx.infoF(_T("Mux完了: %.2f秒"), sw.getAndReset());

    muxer = nullptr;
    thSetPowerThrottling->abortThread();

    // 出力結果を表示
    reformInfo.printOutputMapping([&](EncodeFileKey key) {
        const auto& file = reformInfo.getEncodeFile(key);
        return setting.getOutFilePath(file.outKey, file.keyMax, getActualOutputFormat(key, reformInfo, setting), eoInfo.format);
        });

    // 出力結果JSON出力
    if (setting.getOutInfoJsonPath().size() > 0) {
        StringBuilder sb;
        sb.append("{ ")
            .append("\"srcpath\": \"%s\", ", toJsonString(setting.getSrcFilePath()))
            .append("\"outfiles\": [");
        for (int i = 0; i < (int)keys.size(); i++) {
            if (i > 0) sb.append(", ");
            const auto& file = reformInfo.getEncodeFile(keys[i]);
            const auto& info = outFileInfo[i];
            sb.append("{ \"path\": \"%s\", \"srcbitrate\": %d, \"outbitrate\": %d, \"outfilesize\": %lld, ",
                toJsonString(setting.getOutFilePath(file.outKey, file.keyMax, getActualOutputFormat(keys[i], reformInfo, setting), eoInfo.format)), (int)info.srcBitrate,
                std::isnan(info.targetBitrate) ? -1 : (int)info.targetBitrate, info.fileSize);
            sb.append("\"subs\": [");
            for (int s = 0; s < (int)info.outSubs.size(); s++) {
                if (s > 0) sb.append(", ");
                sb.append("\"%s\"", toJsonString(info.outSubs[s]));
            }
            sb.append("] }");
        }
        sb.append("]")
            .append(", \"logofiles\": [");
        for (int i = 0; i < reformInfo.getNumVideoFile(); i++) {
            if (i > 0) sb.append(", ");
            sb.append("\"%s\"", toJsonString(cmanalyze[i]->getLogoPath()));
        }
        sb.append("]")
            .append(", \"srcfilesize\": %lld, \"intvideofilesize\": %lld, \"outfilesize\": %lld",
                srcFileSize, totalIntVideoSize, totalOutSize);
        auto duration = reformInfo.getInOutDuration();
        sb.append(", \"srcduration\": %.3f, \"outduration\": %.3f",
            (double)duration.first / MPEG_CLOCK_HZ, (double)duration.second / MPEG_CLOCK_HZ);
        sb.append(", \"audiodiff\": ");
        audioDiffInfo.printToJson(sb);
        sb.append(", \"error\": {");
        for (int i = 0; i < AMT_ERR_MAX; i++) {
            if (i > 0) sb.append(", ");
            sb.append("\"%s\": %d", AMT_ERROR_NAMES[i], ctx.getErrorCount((AMT_ERROR_COUNTER)i));
        }
        sb.append(" }");
        sb.append(", \"cmanalyze\": %s", (setting.isChapterEnabled() ? "true" : "false"))
            .append(", \"nicojk\": %s", (nicoOK ? "true" : "false"))
            .append(", \"trimavs\": %s", (setting.getTrimAVSPath().size() ? "true" : "false"))
            .append(" }");

        std::string str = sb.str();
        MemoryChunk mc(reinterpret_cast<uint8_t*>(const_cast<char*>(str.data())), str.size());
        File file(setting.getOutInfoJsonPath(), _T("w"));
        file.write(mc);
    }
}

/* static */ void transcodeSimpleMain(AMTContext& ctx, const ConfigWrapper& setting) {
    if (ends_with(setting.getSrcFilePath(), _T(".ts"))) {
        ctx.warn(_T("一般ファイルモードでのTSファイルの処理は非推奨です"));
    }

    auto encoder = std::unique_ptr<AMTSimpleVideoEncoder>(new AMTSimpleVideoEncoder(ctx, setting));
    encoder->encode();
    int audioCount = encoder->getAudioCount();
    int64_t srcFileSize = encoder->getSrcFileSize();
    VideoFormat videoFormat = encoder->getVideoFormat();
    encoder = nullptr;

    auto muxer = std::unique_ptr<AMTSimpleMuxder>(new AMTSimpleMuxder(ctx, setting));
    muxer->mux(videoFormat, audioCount);
    int64_t totalOutSize = muxer->getTotalOutSize();
    muxer = nullptr;

    // 出力結果を表示
    ctx.info(_T("完了"));
    if (setting.getOutInfoJsonPath().size() > 0) {
        StringBuilder sb;
        sb.append("{ \"srcpath\": \"%s\"", toJsonString(setting.getSrcFilePath()))
            .append(", \"outpath\": \"%s\"", toJsonString(setting.getOutFilePath(EncodeFileKey(), EncodeFileKey(), setting.getFormat(), videoFormat.format)))
            .append(", \"srcfilesize\": %lld", srcFileSize)
            .append(", \"outfilesize\": %lld", totalOutSize)
            .append(" }");

        std::string str = sb.str();
        MemoryChunk mc(reinterpret_cast<uint8_t*>(const_cast<char*>(str.data())), str.size());
        File file(setting.getOutInfoJsonPath(), _T("w"));
        file.write(mc);
    }
}
DrcsSearchSplitter::DrcsSearchSplitter(AMTContext& ctx, const ConfigWrapper& setting)
    : TsSplitter(ctx, true, false, true)
    , setting_(setting) {}

void DrcsSearchSplitter::readAll() {
    enum { BUFSIZE = 4 * 1024 * 1024 };
    auto buffer_ptr = std::unique_ptr<uint8_t[]>(new uint8_t[BUFSIZE]);
    MemoryChunk buffer(buffer_ptr.get(), BUFSIZE);
    File srcfile(setting_.getSrcFilePath(), _T("rb"));
    size_t readBytes;
    do {
        readBytes = srcfile.read(buffer);
        inputTsData(MemoryChunk(buffer.data, readBytes));
    } while (readBytes == buffer.length);
}

// TsSplitter仮想関数 //

/* virtual */ void DrcsSearchSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    // 今の所最初のフレームしか必要ないけど
    for (const VideoFrameInfo& frame : frames) {
        videoFrameList_.push_back(frame);
    }
}

/* virtual */ void DrcsSearchSplitter::onVideoFormatChanged(VideoFormat fmt) {}

/* virtual */ void DrcsSearchSplitter::onAudioPesPacket(
    int audioIdx,
    int64_t clock,
    const std::vector<AudioFrameData>& frames,
    PESPacket packet) {}

/* virtual */ void DrcsSearchSplitter::onAudioFormatChanged(int audioIdx, AudioFormat fmt) {}

/* virtual */ void DrcsSearchSplitter::onCaptionPesPacket(
    int64_t clock,
    std::vector<CaptionItem>& captions,
    PESPacket packet) {}

/* virtual */ DRCSOutInfo DrcsSearchSplitter::getDRCSOutPath(int64_t PTS, const std::string& md5) {
    DRCSOutInfo info;
    info.elapsed = (videoFrameList_.size() > 0) ? (double)(PTS - videoFrameList_[0].PTS) : -1.0;
    info.filename = setting_.getDRCSOutPath(md5);
    return info;
}

/* virtual */ void DrcsSearchSplitter::onTime(int64_t clock, JSTTime time) {}
SubtitleDetectorSplitter::SubtitleDetectorSplitter(AMTContext& ctx, const ConfigWrapper& setting)
    : TsSplitter(ctx, true, false, true)
    , setting_(setting)
    , hasSubtltle_(false) {}

void SubtitleDetectorSplitter::readAll(int maxframes) {
    enum { BUFSIZE = 4 * 1024 * 1024 };
    auto buffer_ptr = std::unique_ptr<uint8_t[]>(new uint8_t[BUFSIZE]);
    MemoryChunk buffer(buffer_ptr.get(), BUFSIZE);
    File srcfile(setting_.getSrcFilePath(), _T("rb"));
    auto fileSize = srcfile.size();
    // ファイル先頭から10%のところから読む
    srcfile.seek(fileSize / 10, SEEK_SET);
    int64_t totalRead = 0;
    // 最後の10%は読まない
    int64_t end = fileSize / 10 * 9;
    size_t readBytes;
    do {
        readBytes = srcfile.read(buffer);
        inputTsData(MemoryChunk(buffer.data, readBytes));
        totalRead += readBytes;
    } while (totalRead < end && !hasSubtltle_ && (int)videoFrameList_.size() < maxframes);
}

bool SubtitleDetectorSplitter::getHasSubtitle() const {
    return hasSubtltle_;
}

// TsSplitter仮想関数 //

/* virtual */ void SubtitleDetectorSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    // 今の所最初のフレームしか必要ないけど
    for (const VideoFrameInfo& frame : frames) {
        videoFrameList_.push_back(frame);
    }
}

/* virtual */ void SubtitleDetectorSplitter::onVideoFormatChanged(VideoFormat fmt) {}

/* virtual */ void SubtitleDetectorSplitter::onAudioPesPacket(
    int audioIdx,
    int64_t clock,
    const std::vector<AudioFrameData>& frames,
    PESPacket packet) {}

/* virtual */ void SubtitleDetectorSplitter::onAudioFormatChanged(int audioIdx, AudioFormat fmt) {}

/* virtual */ void SubtitleDetectorSplitter::onCaptionPesPacket(
    int64_t clock,
    std::vector<CaptionItem>& captions,
    PESPacket packet) {}

/* virtual */ DRCSOutInfo SubtitleDetectorSplitter::getDRCSOutPath(int64_t PTS, const std::string& md5) {
    return DRCSOutInfo();
}

/* virtual */ void SubtitleDetectorSplitter::onTime(int64_t clock, JSTTime time) {}

/* virtual */ void SubtitleDetectorSplitter::onCaptionPacket(int64_t clock, TsPacket packet) {
    hasSubtltle_ = true;
}
AudioDetectorSplitter::AudioDetectorSplitter(AMTContext& ctx, const ConfigWrapper& setting)
    : TsSplitter(ctx, true, true, false)
    , setting_(setting) {}

void AudioDetectorSplitter::readAll(int maxframes) {
    enum { BUFSIZE = 4 * 1024 * 1024 };
    auto buffer_ptr = std::unique_ptr<uint8_t[]>(new uint8_t[BUFSIZE]);
    MemoryChunk buffer(buffer_ptr.get(), BUFSIZE);
    File srcfile(setting_.getSrcFilePath(), _T("rb"));
    auto fileSize = srcfile.size();
    // ファイル先頭から10%のところから読む
    srcfile.seek(fileSize / 10, SEEK_SET);
    int64_t totalRead = 0;
    // 最後の10%は読まない
    int64_t end = fileSize / 10 * 9;
    size_t readBytes;
    do {
        readBytes = srcfile.read(buffer);
        inputTsData(MemoryChunk(buffer.data, readBytes));
        totalRead += readBytes;
    } while (totalRead < end && (int)videoFrameList_.size() < maxframes);
}

// TsSplitter仮想関数 //

/* virtual */ void AudioDetectorSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    // 今の所最初のフレームしか必要ないけど
    for (const VideoFrameInfo& frame : frames) {
        videoFrameList_.push_back(frame);
    }
}

/* virtual */ void AudioDetectorSplitter::onVideoFormatChanged(VideoFormat fmt) {}

/* virtual */ void AudioDetectorSplitter::onAudioPesPacket(
    int audioIdx,
    int64_t clock,
    const std::vector<AudioFrameData>& frames,
    PESPacket packet) {}

/* virtual */ void AudioDetectorSplitter::onAudioFormatChanged(int audioIdx, AudioFormat fmt) {
    _ftprintf(stdout, _T("インデックス: %d チャンネル: %s サンプルレート: %d\n"),
        audioIdx, getAudioChannelString(fmt.channels), fmt.sampleRate);
}

/* virtual */ void AudioDetectorSplitter::onCaptionPesPacket(
    int64_t clock,
    std::vector<CaptionItem>& captions,
    PESPacket packet) {}

/* virtual */ DRCSOutInfo AudioDetectorSplitter::getDRCSOutPath(int64_t PTS, const std::string& md5) {
    return DRCSOutInfo();
}

/* virtual */ void AudioDetectorSplitter::onTime(int64_t clock, JSTTime time) {}

/* static */ void searchDrcsMain(AMTContext& ctx, const ConfigWrapper& setting) {
    Stopwatch sw;
    sw.start();
    auto splitter = std::unique_ptr<DrcsSearchSplitter>(new DrcsSearchSplitter(ctx, setting));
    if (setting.getServiceId() > 0) {
        splitter->setServiceId(setting.getServiceId());
    }
    splitter->readAll();
    ctx.infoF(_T("完了: %.2f秒"), sw.getAndReset());
}

/* static */ void detectSubtitleMain(AMTContext& ctx, const ConfigWrapper& setting) {
    auto splitter = std::unique_ptr<SubtitleDetectorSplitter>(new SubtitleDetectorSplitter(ctx, setting));
    if (setting.getServiceId() > 0) {
        splitter->setServiceId(setting.getServiceId());
    }
    splitter->readAll(setting.getMaxFrames());
    printf("字幕%s\n", splitter->getHasSubtitle() ? "あり" : "なし");
}

/* static */ void detectAudioMain(AMTContext& ctx, const ConfigWrapper& setting) {
    auto splitter = std::unique_ptr<AudioDetectorSplitter>(new AudioDetectorSplitter(ctx, setting));
    if (setting.getServiceId() > 0) {
        splitter->setServiceId(setting.getServiceId());
    }
    splitter->readAll(setting.getMaxFrames());
}
