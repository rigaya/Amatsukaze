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
#include "AdtsParser.h"
#include "PacketCache.h"
#include "rgy_pipe.h"
#include "rgy_mutex.h"
#include "Subtitle.h"
#include "WaveWriter.h"

namespace {

struct WhisperAudioEntry {
    int keyIndex;
    EncodeFileKey key;
    int localIndex;
    tstring audioPath;
    int audioSourceIndex;
    int dualMonoChannel; // -1: original stereo, 0/1: dual mono channel selection
};

static tstring createWhisperWaveInput(AMTContext& ctx,
                                      const ConfigWrapper& setting,
                                      const StreamReformInfo& reformInfo,
                                      const WhisperAudioEntry& entry) {
    const int bytesPerSample = 2;
    const int srcChannels = 2; // AMTSplitter outputs 16bit stereo PCM for whisper input
    const int destChannels = (entry.dualMonoChannel >= 0) ? 1 : srcChannels;

    if (entry.audioSourceIndex < 0) {
        ctx.warn("Whisper入力wav作成: 音声インデックスが不正です");
        return tstring();
    }

    const auto& key = entry.key;
    const auto& fileIn = reformInfo.getEncodeFile(key);
    if (entry.audioSourceIndex >= (int)fileIn.audioFrames.size()) {
        ctx.warnF("Whisper入力wav作成: 音声%d-%dで音声インデックス%dが範囲外です", key.video, key.format, entry.audioSourceIndex);
        return tstring();
    }

    const auto& frameIndexList = fileIn.audioFrames[entry.audioSourceIndex];
    if (frameIndexList.empty()) {
        ctx.warnF("Whisper入力wav作成: 音声%d-%d-%dにフレームが存在しません", key.video, key.format, entry.audioSourceIndex);
        return tstring();
    }

    auto waveFrames = reformInfo.getWaveInput(frameIndexList);
    if (waveFrames.empty()) {
        ctx.warnF("Whisper入力wav作成: 音声%d-%d-%dのwave情報が取得できません", key.video, key.format, entry.audioSourceIndex);
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
        ctx.warnF("Whisper入力wav作成: ファイルを開けません (%s)", wavPath.c_str());
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

} // namespace

AMTSplitter::AMTSplitter(AMTContext& ctx, const ConfigWrapper& setting)
    : TsSplitter(ctx, true, true, setting.isSubtitlesEnabled())
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
    enum { BUFSIZE = 4 * 1024 * 1024 };
    auto buffer_ptr = std::unique_ptr<uint8_t[]>(new uint8_t[BUFSIZE]);
    MemoryChunk buffer(buffer_ptr.get(), BUFSIZE);
    File srcfile(setting_.getSrcFilePath(), _T("rb"));
    // 入力TSそのままのコピーを作成 (WebVTT出力ON または tsreplace時)
    const bool needCopyTS = setting_.isWebVTTEnabled() || (setting_.getFormat() == FORMAT_TSREPLACE);
    std::unique_ptr<File> rawts;
    if (needCopyTS) {
        rawts.reset(new File(setting_.getTmpRawTSPath(), _T("wb")));
    }
    srcFileSize_ = srcfile.size();
    size_t readBytes;
    do {
        readBytes = srcfile.read(buffer);
        if (readBytes > 0 && rawts) {
            rawts->write(MemoryChunk(buffer.data, readBytes));
        }
        inputTsData(MemoryChunk(buffer.data, readBytes));
    } while (readBytes == buffer.length);
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
        ctx.error("フレームがありません");
        return;
    }

    // ラップアラウンドしないPTSを生成
    std::vector<std::pair<int64_t, int>> modifiedPTS;
    int64_t videoBasePTS = videoFrameList_[0].PTS;
    int64_t prevPTS = videoFrameList_[0].PTS;
    for (int i = 0; i < int(videoFrameList_.size()); ++i) {
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
    for (int i = 0; i < (int)modifiedPTS.size(); ++i) {
        int64_t PTS = modifiedPTS[i].first;
        int decodeIndex = modifiedPTS[i].second;
        const VideoFrameInfo& frame = videoFrameList_[decodeIndex];
        int PTSdiff = -1;
        if (i < (int)modifiedPTS.size() - 1) {
            int64_t nextPTS = modifiedPTS[i + 1].first;
            const VideoFrameInfo& nextFrame = videoFrameList_[modifiedPTS[i + 1].second];
            PTSdiff = int(nextPTS - PTS);
            if (CheckPullDown(frame.pic, nextFrame.pic) == false) {
                ctx.warn("Flag Check Error: PTS=%lld %s -> %s",
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

    ctx.info("[映像フレーム統計情報]");

    int64_t totalTime = modifiedPTS.back().first - videoBasePTS;
    double sec = (double)totalTime / MPEG_CLOCK_HZ;
    int minutes = (int)(sec / 60);
    sec -= minutes * 60;
    ctx.infoF("時間: %d分%.3f秒", minutes, sec);

    ctx.infoF("FRAME=%d DBL=%d TLP=%d TFF=%d BFF=%d TFF_RFF=%d BFF_RFF=%d",
        interaceCounter[0], interaceCounter[1], interaceCounter[2], interaceCounter[3], interaceCounter[4], interaceCounter[5], interaceCounter[6]);

    for (const auto& pair : PTSdiffMap) {
        ctx.infoF("(PTS_Diff,Cnt)=(%d,%d)", pair.first, pair.second.v);
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
    ctx.info("[映像フォーマット変更]");

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
    ctx.info(sb.str().c_str());

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
    ctx.infoF("[音声%dフォーマット変更]", audioIdx);
    ctx.infoF("チャンネル: %s サンプルレート: %d",
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
    ret = str_replace(ret, _T("@AMT_ENCODER@"), char_to_tstring(encoderToString(setting.getEncoder())));
    ret = str_replace(ret, _T("@AMT_AUDIO_ENCODER@"), char_to_tstring(audioEncoderToString(setting.getAudioEncoder())));
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
        tmpvar.set(_T("AMT_ENCODER"), char_to_tstring(encoderToString(setting.getEncoder())));
        tmpvar.set(_T("AMT_AUDIO_ENCODER"), char_to_tstring(audioEncoderToString(setting.getAudioEncoder())));
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
    const EncoderOptionInfo& eoInfo) {
    VIDEO_STREAM_FORMAT srcFormat = reformInfo_.getVideoStreamFormat();
    double srcBitrate = getSourceBitrate(key.video);
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
        setting_.getEncVideoFilePath(key));
}

// src, target
std::pair<double, double> EncoderArgumentGenerator::printBitrate(AMTContext& ctx, EncodeFileKey key) const {
    double srcBitrate = getSourceBitrate(key.video);
    ctx.infoF("入力映像ビットレート: %d kbps", (int)srcBitrate);
    VIDEO_STREAM_FORMAT srcFormat = reformInfo_.getVideoStreamFormat();
    double targetBitrate = std::numeric_limits<float>::quiet_NaN();
    if (setting_.isAutoBitrate()) {
        targetBitrate = setting_.getBitrate().getTargetBitrate(srcFormat, srcBitrate);
        if (key.cm == CMTYPE_CM) {
            targetBitrate *= setting_.getBitrateCM();
        }
        ctx.infoF("目標映像ビットレート: %d kbps", (int)targetBitrate);
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
        for (int i = 0; i < (int)cmzones.size(); ++i) {
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

    // チェック
    if (!isNoEncode && !setting.isFormatVFRSupported() && eoInfo.afsTimecode) {
        THROW(FormatException, "M2TS/TS出力はVFRをサポートしていません");
    }
    if (setting.getFormat() == FORMAT_TSREPLACE) {
        auto cmtypes = setting.getCMTypes();
        if (cmtypes.size() != 1 || cmtypes[0] != CMTYPE_BOTH) {
            THROW(FormatException, "tsreplaceはCMカットに対応していません");
        }
        if (eoInfo.format != VS_H264 && eoInfo.format != VS_H265) {
            THROW(FormatException, "tsreplaceはH.264/H.265以外には対応していません");
        }
    }

    ResourceManger rm(ctx, setting.getInPipe(), setting.getOutPipe());
    rm.wait(HOST_CMD_TSAnalyze);

    Stopwatch sw;
    sw.start();
    auto splitter = std::unique_ptr<AMTSplitter>(new AMTSplitter(ctx, setting));
    if (setting.getServiceId() > 0) {
        splitter->setServiceId(setting.getServiceId());
    }
    StreamReformInfo reformInfo = splitter->split();
    ctx.infoF("TS解析完了: %.2f秒", sw.getAndReset());
    const int serviceId = splitter->getActualServiceId();
    const int64_t numTotalPackets = splitter->getNumTotalPackets();
    const int64_t numScramblePackets = splitter->getNumScramblePackets();
    const int64_t totalIntVideoSize = splitter->getTotalIntVideoSize();
    const int64_t srcFileSize = splitter->getSrcFileSize();
    splitter = nullptr;

    if (setting.isDumpStreamInfo()) {
        reformInfo.serialize(setting.getStreamInfoPath());
    }

    // tsreadexでトレースを取得 (WebVTT出力時のみ)
    if (setting.isWebVTTEnabled()) {
        ctx.info("[tsreadex 解析]");
        File stdoutf(setting.getTmpTsReadExDumpPath(), _T("wb"));
        tstring args = StringFormat(_T("\"%s\" -n -1 -r - \"%s\""), setting.getTsReadExPath().c_str(), setting.getTmpRawTSPath().c_str());
        ctx.infoF("tsreadex コマンド: %s", args.c_str());
        class MyTsReadEx : public EventBaseSubProcess {
        public:
            MyTsReadEx(const tstring& args, File* out) : EventBaseSubProcess(args), out(out) {}
        protected:
            File* out;
            virtual void onOut(bool isErr, MemoryChunk mc) {
                if (!isErr && out) out->write(mc);
            }
        };
        MyTsReadEx process(args, &stdoutf);
        int exitCode = process.join();
        if (exitCode != 0) {
            THROWF(FormatException, "tsreadexがエラーコード(%d)を返しました", exitCode);
        }
        ctx.infoF("tsreadex 完了: %.2f秒", sw.getAndReset());
    }

    // スクランブルパケットチェック
    double scrambleRatio = (double)numScramblePackets / (double)numTotalPackets;
    if (scrambleRatio > 0.01) {
        ctx.errorF("%.2f%%のパケットがスクランブル状態です。", scrambleRatio * 100);
        if (scrambleRatio > 0.3) {
            THROW(FormatException, "スクランブルパケットが多すぎます");
        }
    }

    if (!isNoEncode && setting.isIgnoreNoDrcsMap() == false) {
        // DRCSマッピングチェック
        if (ctx.getErrorCount(AMT_ERR_NO_DRCS_MAP) > 0) {
            THROW(NoDrcsMapException, "マッピングにないDRCS外字あり正常に字幕処理できなかったため終了します");
        }
    }

    reformInfo.prepare(setting.isSplitSub(), setting.isEncodeAudio(), setting.getFormat() == FORMAT_TSREPLACE);

    time_t startTime = reformInfo.getFirstFrameTime();

    NicoJK nicoJK(ctx, setting);
    bool nicoOK = false;
    if (!isNoEncode && setting.isNicoJKEnabled()) {
        ctx.info("[ニコニコ実況コメント取得]");
        auto srcDuration = reformInfo.getInDuration() / MPEG_CLOCK_HZ;
        nicoOK = nicoJK.makeASS(serviceId, startTime, (int)srcDuration);
        if (nicoOK) {
            reformInfo.SetNicoJKList(nicoJK.getDialogues());
        } else {
            if (nicoJK.isFail() == false) {
                ctx.info("対応チャンネルがありません");
            } else if (setting.isIgnoreNicoJKError() == false) {
                THROW(RuntimeException, "ニコニコ実況コメント取得に失敗");
            }
        }
    }

    int numVideoFiles = reformInfo.getNumVideoFile();
    int mainFileIndex = reformInfo.getMainVideoFileIndex();
    std::vector<std::unique_ptr<CMAnalyze>> cmanalyze;

    // ソースファイル読み込み用データ保存
    for (int videoFileIndex = 0; videoFileIndex < numVideoFiles; ++videoFileIndex) {
        // ファイル読み込み情報を保存
        auto& fmt = reformInfo.getFormat(EncodeFileKey(videoFileIndex, 0));
        auto amtsPath = setting.getTmpAMTSourcePath(videoFileIndex);
        av::SaveAMTSource(amtsPath,
            setting.getIntVideoFilePath(videoFileIndex),
            setting.getWaveFilePath(),
            fmt.videoFormat, fmt.audioFormat[0],
            reformInfo.getFilterSourceFrames(videoFileIndex),
            reformInfo.getFilterSourceAudioFrames(videoFileIndex),
            setting.getDecoderSetting());
    }

    // ロゴ・CM解析
    rm.wait(HOST_CMD_CMAnalyze);
    sw.start();
    std::vector<std::pair<size_t, bool>> logoFound;
    std::vector<std::unique_ptr<MakeChapter>> chapterMakers(numVideoFiles);
    for (int videoFileIndex = 0; videoFileIndex < numVideoFiles; ++videoFileIndex) {
        cmanalyze.push_back(std::make_unique<CMAnalyze>(ctx, setting));
        const auto& inputVideofmt = reformInfo.getFormat(EncodeFileKey(videoFileIndex, 0)).videoFormat;
        const int numFrames = (int)reformInfo.getFilterSourceFrames(videoFileIndex).size();
        const bool delogoEnabled = setting.isNoDelogo() ? false : true;
        // チャプター解析は300フレーム（約10秒）以上ある場合だけ
        //（短すぎるとエラーになることがあるので
        const bool analyzeChapterAndCM = (setting.isChapterEnabled() && numFrames >= 300);
        CMAnalyze *cma = cmanalyze.back().get();
        if (analyzeChapterAndCM || delogoEnabled) {
            cma->analyze(serviceId, videoFileIndex, inputVideofmt, numFrames, analyzeChapterAndCM);
        }

        if (analyzeChapterAndCM && setting.isPmtCutEnabled()) {
            // PMT変更によるCM追加認識
            cma->applyPmtCut(numFrames, setting.getPmtCutSideRate(),
                reformInfo.getPidChangedList(videoFileIndex));
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
        ctx.infoF("ロゴ・CM解析完了: %.2f秒", sw.getAndReset());
    }

    if (isNoEncode) {
        // CM解析のみならここで終了
        return;
    }

    auto audioDiffInfo = reformInfo.genAudio(setting.getCMTypes());
    audioDiffInfo.printAudioPtsDiff(ctx);

    const auto& allKeys = reformInfo.getOutFileKeys();
    std::vector<EncodeFileKey> keys;
    // 1秒以下なら出力しない
    std::copy_if(allKeys.begin(), allKeys.end(), std::back_inserter(keys),
        [&](EncodeFileKey key) { return reformInfo.getEncodeFile(key).duration >= MPEG_CLOCK_HZ; });

    std::vector<EncodeFileOutput> outFileInfo(keys.size());

    ctx.info("[チャプター生成]");
    for (int i = 0; i < (int)keys.size(); ++i) {
        auto key = keys[i];
        if (chapterMakers[key.video]) {
            chapterMakers[key.video]->exec(key);
        }
    }

    std::vector<WhisperAudioEntry> whisperAudioEntries;
    std::vector<int> whisperLocalIndex(keys.size(), 0);
    if (setting.isEncodeAudio()) {
        ctx.info("[音声エンコード]");
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
        ctx.info("[音声出力]");
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
                        ctx.infoF("音声%d-%dはデュアルモノなので2つのAACファイルに分離します", fileIn.outKey.format, asrc);
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
                    } else {
                        if (isDualMono) {
                            ctx.infoF("音声%d-%dはデュアルモノですが、音声フォーマット無視指定があるので分離しません", fileIn.outKey.format, asrc);
                        }
                        const int adst0 = adst++;
                        const tstring filepath = setting.getIntAudioFilePath(key, adst0, setting.getAudioEncoder());
                        whisperAudioEntries.push_back({ i, key, adst0, filepath, asrc, -1 });
                        File file(filepath, _T("wb"));
                        for (int frameIndex : frameList) {
                            file.write(audioCache[frameIndex]);
                        }
                    }
                }
            }
        }
    }

    ctx.info("[字幕ファイル生成]");
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
            ctx.infoF("字幕ファイル出力: %s", setting.getTmpASSFilePath(key, lang).c_str());
            if (srt.size() > 0) {
                // SRTはCP_STR_SMALLしかなかった場合など出力がない場合があり、
                // 空ファイルはmux時にエラーになるので、1行もない場合は出力しない
                WriteUTF8File(setting.getTmpSRTFilePath(key, lang), srt);
                ctx.infoF("字幕ファイル出力: %s", setting.getTmpSRTFilePath(key, lang).c_str());
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
                reformInfo.genWebVTT(key, setting);
            }
        } catch (const Exception& e) {
            ctx.warnF("WebVTT生成に失敗: %s", e.message());
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
                                    ctx.warnF("whisper-cpp用のwav生成に失敗したため、元の音声を使用します: %s", whisperInput.c_str());
                                }
                            } catch (const Exception& e) {
                                ctx.warnF("whisper-cpp用wav生成中に例外: %s", e.message());
                                ctx.warnF("元の音声を使用します: %s", whisperInput.c_str());
                            }
                        }

                        if (whisperInput.empty()) {
                            ctx.warn("Whisper字幕生成: 音声入力パスが空のためスキップします");
                            continue;
                        }

                        whisperGen.runWhisper(whisper, whisperInput, wdir,
                            setting.getTmpWhisperFilenameWithoutExt(entry.key, entry.localIndex), extraOpt, setting.isWebVTTEnabled(), true);
                        const auto srtPath = setting.getTmpWhisperSrtPath(entry.key, entry.localIndex);
                        const auto vttPath = setting.getTmpWhisperVttPath(entry.key, entry.localIndex);
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
        } catch (const Exception& e) {
            ctx.warnF("Whisper字幕生成に失敗: %s", e.message());
        }
    }
    ctx.infoF("字幕ファイル生成完了: %.2f秒", sw.getAndReset());

    auto argGen = std::unique_ptr<EncoderArgumentGenerator>(new EncoderArgumentGenerator(setting, reformInfo));

    sw.start();
    for (int i = 0; i < (int)keys.size(); i++) {
        auto key = keys[i];
        auto& fileOut = outFileInfo[i];
        const CMAnalyze* cma = cmanalyze[key.video].get();

        AMTFilterSource filterSource(ctx, setting, reformInfo,
            cma->getZones(), cma->getLogoPath(), key, rm);

        if (!setting.getPreEncBatchFile().empty()) {
            ctx.infoF("[エンコード前バッチファイル] %d/%d", i + 1, (int)keys.size());
            ctx.infoF("%s", setting.getPreEncBatchFile().c_str());
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

            ctx.infoF("[エンコード開始] %d/%d %s", i + 1, (int)keys.size(), CMTypeToString(key.cm));
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
                ctx.infoF("VFRタイミング: %d fps", fileOut.vfrTimingFps);
                fileOut.timecode = setting.getAvsTimecodePath(key);
            }
            if (eoInfo.parallel > 1) {
                ctx.infoF("分割エンコード: %d", eoInfo.parallel);
            }

            std::vector<int> pass;
            if (setting.isTwoPass()) {
                pass.push_back(1);
                pass.push_back(2);
            } else {
                pass.push_back(-1);
            }

            auto bitrateZones = MakeBitrateZones(timeCodes, encoderZones, setting, eoInfo, outvi);
            auto vfrBitrateScale = AdjustVFRBitrate(timeCodes, outvi.fps_numerator, outvi.fps_denominator);
            // VFRフレームタイミングが120fpsか
            std::vector<tstring> encoderArgs;
            for (int i = 0; i < (int)pass.size(); i++) {
                encoderArgs.push_back(
                    argGen->GenEncoderOptions(
                        outvi.num_frames,
                        outfmt, bitrateZones, vfrBitrateScale,
                        fileOut.timecode, fileOut.vfrTimingFps, key, pass[i], serviceId, eoInfo));
            }
            // x264, x265, SVT-AV1のときはdisablePowerThrottoling=trueとする
            // QSV/NV/VCEEncではプロセス内で自動的に最適なように設定されるため不要
            const bool disablePowerThrottoling = (setting.getEncoder() == ENCODER_X264 || setting.getEncoder() == ENCODER_X265 || setting.getEncoder() == ENCODER_SVTAV1);
            AMTFilterVideoEncoder encoder(ctx, std::max(4, setting.getNumEncodeBufferFrames()));
            // 並列GetFrame用にフィルタチェーンを構築するファクトリを渡す
            // 既存のfilterSourceの前処理結果（スクリプト）を再利用して環境を再構築
            auto filterFactory = [&]() -> std::unique_ptr<AMTFilterSource> {
                return std::unique_ptr<AMTFilterSource>(new AMTFilterSource(ctx, filterSource));
            };
            encoder.encode(filterClip, outfmt,
                timeCodes, encoderArgs, eoInfo.parallel, disablePowerThrottoling, env, filterFactory);
        } catch (const AvisynthError& avserror) {
            THROWF(AviSynthException, "%s", avserror.msg);
        }
    }
    ctx.infoF("エンコード完了: %.2f秒", sw.getAndReset());

    argGen = nullptr;

    rm.wait(HOST_CMD_Mux);
    sw.start();
    int64_t totalOutSize = 0;
    auto muxer = std::unique_ptr<AMTMuxder>(new AMTMuxder(ctx, setting, reformInfo));
    for (int i = 0; i < (int)keys.size(); i++) {
        auto key = keys[i];

        ctx.infoF("[Mux開始] %d/%d %s", i + 1, (int)keys.size(), CMTypeToString(key.cm));
        muxer->mux(key, eoInfo, nicoOK, outFileInfo[i]);

        totalOutSize += outFileInfo[i].fileSize;
    }
    ctx.infoF("Mux完了: %.2f秒", sw.getAndReset());

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
        for (int i = 0; i < (int)keys.size(); ++i) {
            if (i > 0) sb.append(", ");
            const auto& file = reformInfo.getEncodeFile(keys[i]);
            const auto& info = outFileInfo[i];
            sb.append("{ \"path\": \"%s\", \"srcbitrate\": %d, \"outbitrate\": %d, \"outfilesize\": %lld, ",
                toJsonString(setting.getOutFilePath(file.outKey, file.keyMax, getActualOutputFormat(keys[i], reformInfo, setting), eoInfo.format)), (int)info.srcBitrate,
                std::isnan(info.targetBitrate) ? -1 : (int)info.targetBitrate, info.fileSize);
            sb.append("\"subs\": [");
            for (int s = 0; s < (int)info.outSubs.size(); ++s) {
                if (s > 0) sb.append(", ");
                sb.append("\"%s\"", toJsonString(info.outSubs[s]));
            }
            sb.append("] }");
        }
        sb.append("]")
            .append(", \"logofiles\": [");
        for (int i = 0; i < reformInfo.getNumVideoFile(); ++i) {
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
        for (int i = 0; i < AMT_ERR_MAX; ++i) {
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
        ctx.warn("一般ファイルモードでのTSファイルの処理は非推奨です");
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
    ctx.info("完了");
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
    printf("インデックス: %d チャンネル: %s サンプルレート: %d\n",
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
    ctx.infoF("完了: %.2f秒", sw.getAndReset());
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
