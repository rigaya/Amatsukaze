/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "Muxer.h"

/* static */ ENUM_FORMAT getActualOutputFormat(EncodeFileKey key, const StreamReformInfo& reformInfo, const ConfigWrapper& setting) {
    if (!setting.getUseMKVWhenSubExist() || setting.getFormat() == FORMAT_MKV) {
        return setting.getFormat();
    }
    bool subExists = false;
    for (NicoJKType jktype : setting.getNicoJKTypes()) {
        auto srcsub = setting.getTmpNicoJKASSPath(key, jktype);
        if (File::exists(srcsub)) {
            subExists = true;
        }
    }
    for (int lang = 0; lang < (int)reformInfo.getEncodeFile(key).captionList.size(); ++lang) {
        auto srcass = setting.getTmpASSFilePath(key, lang);
        if (File::exists(srcass)) {
            subExists = true;
        }
    }
    if (subExists) {
        return FORMAT_MKV;
    }
    return setting.getFormat();
}
AMTMuxder::AMTMuxder(
    AMTContext&ctx,
    const ConfigWrapper& setting,
    const StreamReformInfo& reformInfo)
    : AMTObject(ctx)
    , setting_(setting)
    , reformInfo_(reformInfo) {}

void AMTMuxder::mux(EncodeFileKey key,
    const EncoderOptionInfo& eoInfo, // エンコーダオプション情報
    bool nicoOK,
    EncodeFileOutput& fileOut) // 出力情報
{
    const auto& fileIn = reformInfo_.getEncodeFile(key);
    auto fmt = reformInfo_.getFormat(key);
    auto muxFormat = getActualOutputFormat(key, reformInfo_, setting_);
    auto vfmt = fileOut.vfmt;

    if (eoInfo.selectEvery > 1) {
        // エンコーダで間引く場合があるので、それを反映する
        vfmt.mulDivFps(1, eoInfo.selectEvery);
    }

    if (vfmt.progressive == false) {
        // エンコーダでインタレ解除している場合があるので、それを反映する
        switch (eoInfo.deint) {
        case ENCODER_DEINT_24P:
            vfmt.mulDivFps(4, 5);
            vfmt.progressive = true;
            break;
        case ENCODER_DEINT_30P:
        case ENCODER_DEINT_VFR:
            vfmt.progressive = true;
            break;
        case ENCODER_DEINT_60P:
            vfmt.mulDivFps(2, 1);
            vfmt.progressive = true;
            break;
        default:
            break;
        }
    } else {
        if (eoInfo.deint != ENCODER_DEINT_NONE) {
            // 一応警告を出す
            ctx.warn("エンコーダへの入力はプログレッシブですが、"
                     "エンコーダオプションでインタレ解除指定がされています。");
            ctx.warn("エンコーダでこのオプションが無視される場合は問題ありません。");
        }
    }

    // 音声ファイルを作成
    std::vector<tstring> audioFiles;
    if (setting_.isEncodeAudio()) {
        audioFiles.push_back(setting_.getIntAudioFilePath(key, 0, setting_.getAudioEncoder()));
    } else if (setting_.getFormat() != FORMAT_TSREPLACE) { // tsreaplceの場合は音声ファイルを作らない
        for (int asrc = 0, adst = 0; asrc < (int)fileIn.audioFrames.size(); asrc++) {
            const std::vector<int>& frameList = fileIn.audioFrames[asrc];
            if (frameList.size() > 0) {
                bool isDualMono = (fmt.audioFormat[asrc].channels == AUDIO_2LANG);
                if (!setting_.isEncodeAudio() && isDualMono) {
                    tstring filepath0 = setting_.getIntAudioFilePath(key, adst++, setting_.getAudioEncoder());
                    tstring filepath1 = setting_.getIntAudioFilePath(key, adst++, setting_.getAudioEncoder());
                    audioFiles.push_back(filepath0);
                    audioFiles.push_back(filepath1);
                } else {
                    tstring filepath = setting_.getIntAudioFilePath(key, adst++, setting_.getAudioEncoder());
                    audioFiles.push_back(filepath);
                }
            }
        }
    }

    // 映像ファイル
    tstring encVideoFile;
    encVideoFile = setting_.getEncVideoFilePath(key);

    // チャプターファイル
    tstring chapterFile;
    if (setting_.isChapterEnabled()) {
        auto path = setting_.getTmpChapterPath(key);
        if (File::exists(path)) {
            chapterFile = path;
            if (muxFormat == FORMAT_TSREPLACE || setting_.isOutputChapterEnabled()) {
                //tsreplaceの場合、または--output-chapterが指定された場合は、チャプターファイルを別ファイルとしてコピー
                auto dstchapter = setting_.getOutChapterPath(fileIn.outKey, fileIn.keyMax, muxFormat, eoInfo.format);
                File::copy(chapterFile, dstchapter);
            }
        }
    }

    // 字幕ファイル
    std::vector<tstring> subsFiles;
    std::vector<tstring> subsTitles;
    if (nicoOK) {
        for (NicoJKType jktype : setting_.getNicoJKTypes()) {
            auto srcsub = setting_.getTmpNicoJKASSPath(key, jktype);
            if (muxFormat == FORMAT_MKV) {
                subsFiles.push_back(srcsub);
                subsTitles.push_back(StringFormat(_T("NicoJK%s"), GetNicoJKSuffix(jktype)));
            } else { // MP4の場合は別ファイルとしてコピー
                auto dstsub = setting_.getOutASSPath(fileIn.outKey, fileIn.keyMax, muxFormat, eoInfo.format, -1, jktype);
                File::copy(srcsub, dstsub);
                fileOut.outSubs.push_back(dstsub);
            }
        }
    }
    // ARIB生成字幕 (ASS/SRT/WebVTT)
    if (setting_.getSubtitleMode() == SUBMODE_ARIB || setting_.getSubtitleMode() == SUBMODE_WHISPER_FALLBACK) {
        for (int lang = 0; lang < (int)fileIn.captionList.size(); ++lang) {
            auto srcass = setting_.getTmpASSFilePath(key, lang);
            if (muxFormat == FORMAT_MKV) {
                subsFiles.push_back(srcass);
                subsTitles.push_back(_T("ASS"));
            } else { // MP4,M2TSの場合は別ファイルとしてコピー
                auto dstsub = setting_.getOutASSPath(fileIn.outKey, fileIn.keyMax, muxFormat, eoInfo.format, lang, (NicoJKType)0);
                File::copy(srcass, dstsub);
                fileOut.outSubs.push_back(dstsub);
            }
            auto srcsrt = setting_.getTmpSRTFilePath(key, lang);
            if (File::exists(srcsrt)) {
                subsFiles.push_back(srcsrt);
                subsTitles.push_back(_T("SRT"));
            }
            auto srcwebvtt = setting_.getTmpVTTFilePath(key, lang);
            if (File::exists(srcwebvtt)) {
                auto dstwebvtt = setting_.getOutWebVTTPath(fileIn.outKey, fileIn.keyMax, muxFormat, eoInfo.format, lang);
                File::copy(srcwebvtt, dstwebvtt);
                fileOut.outSubs.push_back(dstwebvtt);
            }
        }
    }

    // Whisper生成字幕 (SRTのみmux対象、VTTはコピーのみ)
    if (setting_.getSubtitleMode() == SUBMODE_WHISPER_ALWAYS || setting_.getSubtitleMode() == SUBMODE_WHISPER_FALLBACK) {
        // 既に作成済みのaudioFiles（dual mono分離後を反映）に合わせて処理
        for (int aindex = 0; aindex < (int)audioFiles.size(); aindex++) {
            auto srcsrt = setting_.getTmpWhisperSrtPath(key, aindex);
            if (File::exists(srcsrt)) {
                subsFiles.push_back(srcsrt);
                subsTitles.push_back(_T("SRT"));
            }
            auto srcvtt = setting_.getTmpWhisperVttPath(key, aindex);
            if (File::exists(srcvtt)) {
                auto dstvtt = setting_.getOutWebVTTPath(fileIn.outKey, fileIn.keyMax, muxFormat, eoInfo.format, aindex);
                File::copy(srcvtt, dstvtt);
                fileOut.outSubs.push_back(dstvtt);
            }
        }
    }
    // pscファイルは別ファイルとしてコピー
    auto srcpsc = setting_.getTmpPSCFilePath(key);
    if (File::exists(srcpsc)) {
        auto dstpsc = setting_.getOutPSCFilePath(fileIn.outKey, fileIn.keyMax, muxFormat, eoInfo.format);
        File::copy(srcpsc, dstpsc);
    }

    const tstring tmpOut1Path = setting_.getVfrTmpFile1Path(key, (muxFormat == FORMAT_TSREPLACE) ? FORMAT_MP4 : muxFormat);
    const tstring tmpOut2Path = setting_.getVfrTmpFile2Path(key, (muxFormat == FORMAT_TSREPLACE) ? FORMAT_MP4 : muxFormat);

    tstring metaFile;
    if (muxFormat == FORMAT_M2TS || muxFormat == FORMAT_TS) {
        // M2TS/TSの場合はmetaファイル作成
        StringBuilder sb;
        sb.append("MUXOPT\n");
        switch (eoInfo.format) {
        case VS_MPEG2:
            sb.append("V_MPEG-2");
            break;
        case VS_H264:
            sb.append("V_MPEG4/ISO/AVC");
            break;
        case VS_H265:
            sb.append("V_MPEGH/ISO/HEVC");
            break;
        default:
            break;
        }
        double fps = vfmt.frameRateNum / (double)vfmt.frameRateDenom;
        sb.append(", \"%s\", fps=%.3f\n", encVideoFile, fps);
        for (auto apath : audioFiles) {
            sb.append("A_AAC, \"%s\"\n", apath);
        }
        for (auto spath : subsFiles) {
            sb.append("S_TEXT/UTF8, \"%s\", fps=%.3f, video-width=%d, video-height=%d\n",
                spath, fps, vfmt.width, vfmt.height);
        }

        metaFile = setting_.getM2tsMetaFilePath(key);
        File file(metaFile, _T("w"));
        file.write(sb.getMC());
    }

    // タイムコード用
    auto timebase = std::make_pair(vfmt.frameRateNum * (fileOut.vfrTimingFps / 30), vfmt.frameRateDenom);

    auto outPath = setting_.getOutFilePath(fileIn.outKey, fileIn.keyMax, muxFormat, eoInfo.format);
    auto muxerPath = setting_.getMuxerPath();
    if (muxFormat != setting_.getFormat()) { // 初期のフォーマットから変わっているとき
        if (muxFormat == FORMAT_MKV) { // useMKVWhenSubExistの場合
            muxerPath = setting_.getMkvMergePath();
            ctx.infoF("字幕が存在するため、mkv出力に切り替えます。");
        } else {
            THROWF(RuntimeException, "Unexpected error, muxFormat != setting_.getFormat()");
        }
    }
    auto args = makeMuxerArgs(
        setting_.getEncoder(), setting_.getUserSAR(), muxFormat, muxerPath,
        setting_.getTimelineEditorPath(), setting_.getMp4BoxPath(),
        (File::exists(setting_.getTmpRawTSPath()) ? setting_.getTmpRawTSPath() : setting_.getSrcFilePath()),
        encVideoFile, encoderOutputInContainer(setting_.getEncoder(), muxFormat),
        vfmt, audioFiles, setting_.getTmpDir(),
        outPath, tmpOut1Path, tmpOut2Path, chapterFile,
        fileOut.timecode, timebase, subsFiles, subsTitles, metaFile,
        setting_.getTsreplaceRemoveTypeD());

    for (int i = 0; i < (int)args.size(); ++i) {
        ctx.infoF("%s", args[i].first);
        StdRedirectedSubProcess muxer(args[i].first, 0, args[i].second);
        int ret = muxer.join();
        if (ret != 0) {
            THROWF(RuntimeException, "mux failed (exit code: %d)", ret);
        }
        // mp4boxがコンソール出力のコードページを変えてしまうので戻す
        ctx.setDefaultCP();
    }

    File outfile(outPath, _T("rb"));
    fileOut.fileSize = outfile.size();
}
SpDualMonoSplitter::SpDualMonoSplitter(AMTContext& ctx) : DualMonoSplitter(ctx) {}
void SpDualMonoSplitter::open(int index, const tstring& filename) {
    file[index] = std::unique_ptr<File>(new File(filename, _T("wb")));
}
/* virtual */ void SpDualMonoSplitter::OnOutFrame(int index, MemoryChunk mc) {
    file[index]->write(mc);
}
AMTSimpleMuxder::AMTSimpleMuxder(
    AMTContext&ctx,
    const ConfigWrapper& setting)
    : AMTObject(ctx)
    , setting_(setting)
    , totalOutSize_(0) {}

void AMTSimpleMuxder::mux(VideoFormat videoFormat, int audioCount) {
    // Mux
    std::vector<tstring> audioFiles;
    for (int i = 0; i < audioCount; ++i) {
        audioFiles.push_back(setting_.getIntAudioFilePath(EncodeFileKey(), i, setting_.getAudioEncoder()));
    }
    tstring encVideoFile = setting_.getEncVideoFilePath(EncodeFileKey());
    tstring outFilePath = setting_.getOutFilePath(EncodeFileKey(), EncodeFileKey(), setting_.getFormat(), videoFormat.format);
    auto args = makeMuxerArgs(
        setting_.getEncoder(), setting_.getUserSAR(), setting_.getFormat(),
        setting_.getMuxerPath(), setting_.getTimelineEditorPath(), setting_.getMp4BoxPath(),
        setting_.getSrcFilePath(),
        encVideoFile, encoderOutputInContainer(setting_.getEncoder(), setting_.getFormat()),
        videoFormat, audioFiles, setting_.getTmpDir(), outFilePath,
        tstring(), tstring(), tstring(), tstring(), std::pair<int, int>(),
        std::vector<tstring>(), std::vector<tstring>(), tstring(), false);
    ctx.info("[Mux開始]");
    ctx.infoF("%s", args[0].first);

    {
        MySubProcess muxer(args[0].first);
        int ret = muxer.join();
        if (ret != 0) {
            THROWF(RuntimeException, "mux failed (muxer exit code: %d)", ret);
        }
    }

    { // 出力サイズ取得
        File outfile(setting_.getOutFilePath(EncodeFileKey(), EncodeFileKey(), setting_.getFormat(), videoFormat.format), _T("rb"));
        totalOutSize_ += outfile.size();
    }
}

int64_t AMTSimpleMuxder::getTotalOutSize() const {
    return totalOutSize_;
}
AMTSimpleMuxder::MySubProcess::MySubProcess(const tstring& args) : EventBaseSubProcess(args) {}
/* virtual */ void AMTSimpleMuxder::MySubProcess::onOut(bool isErr, MemoryChunk mc) {
    // これはマルチスレッドで呼ばれるの注意
    fwrite(mc.data, mc.length, 1, SUBPROC_OUT);
    fflush(SUBPROC_OUT);
}
