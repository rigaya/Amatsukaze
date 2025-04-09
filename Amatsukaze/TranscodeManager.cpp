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
    srcFileSize_ = srcfile.size();
    size_t readBytes;
    do {
        readBytes = srcfile.read(buffer);
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
    default: // ����ȊO�̓`�F�b�N�ΏۊO
        return true;
    }
}

void AMTSplitter::printInteraceCount() {

    if (videoFrameList_.size() == 0) {
        ctx.error("�t���[��������܂���");
        return;
    }

    // ���b�v�A���E���h���Ȃ�PTS�𐶐�
    std::vector<std::pair<int64_t, int>> modifiedPTS;
    int64_t videoBasePTS = videoFrameList_[0].PTS;
    int64_t prevPTS = videoFrameList_[0].PTS;
    for (int i = 0; i < int(videoFrameList_.size()); ++i) {
        int64_t PTS = videoFrameList_[i].PTS;
        int64_t modPTS = prevPTS + int64_t((int32_t(PTS) - int32_t(prevPTS)));
        modifiedPTS.emplace_back(modPTS, i);
        prevPTS = modPTS;
    }

    // PTS�Ń\�[�g
    std::sort(modifiedPTS.begin(), modifiedPTS.end());

#if 0
    // �t���[�����X�g���o��
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

    // PTS�Ԋu���o��
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

    ctx.info("[�f���t���[�����v���]");

    int64_t totalTime = modifiedPTS.back().first - videoBasePTS;
    double sec = (double)totalTime / MPEG_CLOCK_HZ;
    int minutes = (int)(sec / 60);
    sec -= minutes * 60;
    ctx.infoF("����: %d��%.3f�b", minutes, sec);

    ctx.infoF("FRAME=%d DBL=%d TLP=%d TFF=%d BFF=%d TFF_RFF=%d BFF_RFF=%d",
        interaceCounter[0], interaceCounter[1], interaceCounter[2], interaceCounter[3], interaceCounter[4], interaceCounter[5], interaceCounter[6]);

    for (const auto& pair : PTSdiffMap) {
        ctx.infoF("(PTS_Diff,Cnt)=(%d,%d)", pair.first, pair.second.v);
    }
}

// TsSplitter���z�֐� //

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
    ctx.info("[�f���t�H�[�}�b�g�ύX]");

    StringBuilder sb;
    sb.append("�T�C�Y: %dx%d", fmt.width, fmt.height);
    if (fmt.width != fmt.displayWidth || fmt.height != fmt.displayHeight) {
        sb.append(" �\���̈�: %dx%d", fmt.displayWidth, fmt.displayHeight);
    }
    int darW, darH; fmt.getDAR(darW, darH);
    sb.append(" (%d:%d)", darW, darH);
    if (fmt.fixedFrameRate) {
        sb.append(" FPS: %d/%d", fmt.frameRateNum, fmt.frameRateDenom);
    } else {
        sb.append(" FPS: VFR");
    }
    ctx.info(sb.str().c_str());

    // �t�@�C���ύX
    if (!curVideoFormat_.isBasicEquals(fmt)) {
        // �A�X�y�N�g��ȊO���ύX����Ă�����t�@�C���𕪂���
        //�iStreamReform�Ə��������킹�Ȃ���΂Ȃ�Ȃ����Ƃɒ��Ӂj
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
    ctx.infoF("[����%d�t�H�[�}�b�g�ύX]", audioIdx);
    ctx.infoF("�`�����l��: %s �T���v�����[�g: %d",
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

// TsPacketSelectorHandler���z�֐� //

/* virtual */ void AMTSplitter::onPidTableChanged(const PMTESInfo video, const std::vector<PMTESInfo>& audio, const PMTESInfo caption) {
    // �x�[�X�N���X�̏���
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
    ret = str_replace(ret, _T("@AMT_ENCODER@"), to_tstring(encoderToString(setting.getEncoder())));
    ret = str_replace(ret, _T("@AMT_AUDIO_ENCODER@"), to_tstring(audioEncoderToString(setting.getAudioEncoder())));
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
        return _T("");  // �f�B���N�g����؂肪������Ȃ��ꍇ�͋󕶎����Ԃ�
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
    
    // �ꎞ�o�b�`�t�@�C���̃p�X�𐶐�
    tstring tempBatchPath = setting.getEncVideoFilePath(key) + _T(".bat");
    
    try {
        // �o�b�`�t�@�C�����R�s�[
        if (CopyFile(batchPath.c_str(), tempBatchPath.c_str(), FALSE) == 0) {
            return -1;
        }

        const auto outPath = setting.getOutFileBaseWithoutPrefix() + _T(".") + setting.getOutputExtention(setting.getFormat());

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
        tmpvar.set(_T("AMT_ENCODER"), to_tstring(encoderToString(setting.getEncoder())));
        tmpvar.set(_T("AMT_AUDIO_ENCODER"), to_tstring(audioEncoderToString(setting.getAudioEncoder())));
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
        
        // �o�b�`�t�@�C�������s
        STARTUPINFO si = { sizeof(STARTUPINFO) };
        PROCESS_INFORMATION pi;
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_MINIMIZE;
        
        if (!CreateProcess(
            NULL,
            (LPTSTR)tempBatchPath.c_str(),
            NULL,
            NULL,
            FALSE,
            CREATE_NEW_CONSOLE,
            NULL,
            setting.getTmpDir().c_str(),
            &si,
            &pi)) {
            return -1;
        }
        
        // �v���Z�X�̏I����ҋ@
        WaitForSingleObject(pi.hProcess, INFINITE);
        
        // �I���R�[�h���擾
        DWORD exitCode;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        
        // �n���h�������
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        return (int)exitCode;
        
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
    ctx.infoF("���͉f���r�b�g���[�g: %d kbps", (int)srcBitrate);
    VIDEO_STREAM_FORMAT srcFormat = reformInfo_.getVideoStreamFormat();
    double targetBitrate = std::numeric_limits<float>::quiet_NaN();
    if (setting_.isAutoBitrate()) {
        targetBitrate = setting_.getBitrate().getTargetBitrate(srcFormat, srcBitrate);
        if (key.cm == CMTYPE_CM) {
            targetBitrate *= setting_.getBitrateCM();
        }
        ctx.infoF("�ڕW�f���r�b�g���[�g: %d kbps", (int)targetBitrate);
    }
    return std::make_pair(srcBitrate, targetBitrate);
}

double EncoderArgumentGenerator::getSourceBitrate(int fileId) const {
    // �r�b�g���[�g�v�Z
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
        // VFR�łȂ��A�܂��́A�G���R�[�_��VFR���T�|�[�g���Ă��� -> VFR�p�ɒ�������K�v���Ȃ�
        for (int i = 0; i < (int)cmzones.size(); ++i) {
            bitrateZones.emplace_back(cmzones[i], setting.getBitrateCM(), setting.getCMQualityOffset());
        }
    } else {
        if (setting.isZoneAvailable()) {
            // VFR��Ή��G���R�[�_�Ń]�[���ɑΉ����Ă���΃r�b�g���[�g�]�[������
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
                setting.isZoneWithQualityAvailable()   // �i���I�t�Z�b�g��--dynamic-rc�Ŏw��\�ȃG���R�[�_�ł���
                && !setting.isAutoBitrate()            // �����r�b�g���[�g�łȂ�
                && rcMode && !rcMode->isBitrateMode    // �r�b�g���[�g���[�h�łȂ�
                && setting.getCMQualityOffset() != 0.0 // �i���I�t�Z�b�g���L��
            ) {
                for (int i = 0; i < (int)cmzones.size(); i++) {
                    bitrateZones.emplace_back(cmzones[i], setting.getBitrateCM(), setting.getCMQualityOffset());
                }
            } else {
                return MakeVFRBitrateZones(
                    timeCodes, cmzones, setting.getBitrateCM(),
                    outvi.fps_numerator, outvi.fps_denominator,
                    setting.getX265TimeFactor(), 0.05); // �S�̂�5%�܂ł̍��Ȃ狖�e����
            }
        }
    }
    return bitrateZones;
}

// �y�[�W�q�[�v���@�\���Ă��邩�e�X�g
void DoBadThing() {
    char *p = (char*)HeapAlloc(
        GetProcessHeap(),
        HEAP_GENERATE_EXCEPTIONS | HEAP_ZERO_MEMORY,
        8);
    memset(p, 'x', 32);
}
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

    // �`�F�b�N
    if (!isNoEncode && !setting.isFormatVFRSupported() && eoInfo.afsTimecode) {
        THROW(FormatException, "M2TS/TS�o�͂�VFR���T�|�[�g���Ă��܂���");
    }
    if (setting.getFormat() == FORMAT_TSREPLACE) {
        auto cmtypes = setting.getCMTypes();
        if (cmtypes.size() != 1 || cmtypes[0] != CMTYPE_BOTH) {
            THROW(FormatException, "tsreplace��CM�J�b�g�ɑΉ����Ă��܂���");
        }
        if (eoInfo.format != VS_H264 && eoInfo.format != VS_H265) {
            THROW(FormatException, "tsreplace��H.264/H.265�ȊO�ɂ͑Ή����Ă��܂���");
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
    ctx.infoF("TS��͊���: %.2f�b", sw.getAndReset());
    const int serviceId = splitter->getActualServiceId();
    const int64_t numTotalPackets = splitter->getNumTotalPackets();
    const int64_t numScramblePackets = splitter->getNumScramblePackets();
    const int64_t totalIntVideoSize = splitter->getTotalIntVideoSize();
    const int64_t srcFileSize = splitter->getSrcFileSize();
    splitter = nullptr;

    if (setting.isDumpStreamInfo()) {
        reformInfo.serialize(setting.getStreamInfoPath());
    }

    // �X�N�����u���p�P�b�g�`�F�b�N
    double scrambleRatio = (double)numScramblePackets / (double)numTotalPackets;
    if (scrambleRatio > 0.01) {
        ctx.errorF("%.2f%%�̃p�P�b�g���X�N�����u����Ԃł��B", scrambleRatio * 100);
        if (scrambleRatio > 0.3) {
            THROW(FormatException, "�X�N�����u���p�P�b�g���������܂�");
        }
    }

    if (!isNoEncode && setting.isIgnoreNoDrcsMap() == false) {
        // DRCS�}�b�s���O�`�F�b�N
        if (ctx.getErrorCount(AMT_ERR_NO_DRCS_MAP) > 0) {
            THROW(NoDrcsMapException, "�}�b�s���O�ɂȂ�DRCS�O�����萳��Ɏ��������ł��Ȃ��������ߏI�����܂�");
        }
    }

    reformInfo.prepare(setting.isSplitSub(), setting.isEncodeAudio(), setting.getFormat() == FORMAT_TSREPLACE);

    time_t startTime = reformInfo.getFirstFrameTime();

    NicoJK nicoJK(ctx, setting);
    bool nicoOK = false;
    if (!isNoEncode && setting.isNicoJKEnabled()) {
        ctx.info("[�j�R�j�R�����R�����g�擾]");
        auto srcDuration = reformInfo.getInDuration() / MPEG_CLOCK_HZ;
        nicoOK = nicoJK.makeASS(serviceId, startTime, (int)srcDuration);
        if (nicoOK) {
            reformInfo.SetNicoJKList(nicoJK.getDialogues());
        } else {
            if (nicoJK.isFail() == false) {
                ctx.info("�Ή��`�����l��������܂���");
            } else if (setting.isIgnoreNicoJKError() == false) {
                THROW(RuntimeException, "�j�R�j�R�����R�����g�擾�Ɏ��s");
            }
        }
    }

    int numVideoFiles = reformInfo.getNumVideoFile();
    int mainFileIndex = reformInfo.getMainVideoFileIndex();
    std::vector<std::unique_ptr<CMAnalyze>> cmanalyze;

    // �\�[�X�t�@�C���ǂݍ��ݗp�f�[�^�ۑ�
    for (int videoFileIndex = 0; videoFileIndex < numVideoFiles; ++videoFileIndex) {
        // �t�@�C���ǂݍ��ݏ���ۑ�
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

    // ���S�ECM���
    rm.wait(HOST_CMD_CMAnalyze);
    sw.start();
    std::vector<std::pair<size_t, bool>> logoFound;
    std::vector<std::unique_ptr<MakeChapter>> chapterMakers(numVideoFiles);
    for (int videoFileIndex = 0; videoFileIndex < numVideoFiles; ++videoFileIndex) {
        cmanalyze.push_back(std::make_unique<CMAnalyze>(ctx, setting));
        const auto& inputVideofmt = reformInfo.getFormat(EncodeFileKey(videoFileIndex, 0)).videoFormat;
        const int numFrames = (int)reformInfo.getFilterSourceFrames(videoFileIndex).size();
        const bool delogoEnabled = setting.isNoDelogo() ? false : true;
        // �`���v�^�[��͂�300�t���[���i��10�b�j�ȏ゠��ꍇ����
        //�i�Z������ƃG���[�ɂȂ邱�Ƃ�����̂�
        const bool analyzeChapterAndCM = (setting.isChapterEnabled() && numFrames >= 300);
        CMAnalyze *cma = cmanalyze.back().get();
        if (analyzeChapterAndCM || delogoEnabled) {
            cma->analyze(serviceId, videoFileIndex, inputVideofmt, numFrames, analyzeChapterAndCM);
        }

        if (analyzeChapterAndCM && setting.isPmtCutEnabled()) {
            // PMT�ύX�ɂ��CM�ǉ��F��
            cma->applyPmtCut(numFrames, setting.getPmtCutSideRate(),
                reformInfo.getPidChangedList(videoFileIndex));
        }

        if (videoFileIndex == mainFileIndex) {
            if (setting.getTrimAVSPath().size()) {
                // Trim������
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
        // ���S�����������`�F�b�N //
        // �f���t�@�C�����t���[�����Ń\�[�g
        std::sort(logoFound.begin(), logoFound.end());
        if (setting.getLogoPath().size() > 0 && // ���S�w�肠��
            setting.isIgnoreNoLogo() == false &&          // ���S�Ȃ������łȂ�
            logoFound.back().first >= 300 &&
            logoFound.back().second == false)     // �ł������f���Ń��S��������Ȃ�����
        {
            THROW(NoLogoException, "�}�b�`���郍�S��������܂���ł���");
        }
        ctx.infoF("���S�ECM��͊���: %.2f�b", sw.getAndReset());
    }

    if (isNoEncode) {
        // CM��݂͂̂Ȃ炱���ŏI��
        return;
    }

    auto audioDiffInfo = reformInfo.genAudio(setting.getCMTypes());
    audioDiffInfo.printAudioPtsDiff(ctx);

    const auto& allKeys = reformInfo.getOutFileKeys();
    std::vector<EncodeFileKey> keys;
    // 1�b�ȉ��Ȃ�o�͂��Ȃ�
    std::copy_if(allKeys.begin(), allKeys.end(), std::back_inserter(keys),
        [&](EncodeFileKey key) { return reformInfo.getEncodeFile(key).duration >= MPEG_CLOCK_HZ; });

    std::vector<EncodeFileOutput> outFileInfo(keys.size());

    ctx.info("[�`���v�^�[����]");
    for (int i = 0; i < (int)keys.size(); ++i) {
        auto key = keys[i];
        if (chapterMakers[key.video]) {
            chapterMakers[key.video]->exec(key);
        }
    }

    ctx.info("[�����t�@�C������]");
    for (int i = 0; i < (int)keys.size(); ++i) {
        auto key = keys[i];
        CaptionASSFormatter formatterASS(ctx);
        CaptionSRTFormatter formatterSRT(ctx);
        NicoJKFormatter formatterNicoJK(ctx);
        const auto& capList = reformInfo.getEncodeFile(key).captionList;
        for (int lang = 0; lang < capList.size(); ++lang) {
            auto ass = formatterASS.generate(capList[lang]);
            auto srt = formatterSRT.generate(capList[lang]);
            WriteUTF8File(setting.getTmpASSFilePath(key, lang), ass);
            if (srt.size() > 0) {
                // SRT��CP_STR_SMALL�����Ȃ������ꍇ�ȂǏo�͂��Ȃ��ꍇ������A
                // ��t�@�C����mux���ɃG���[�ɂȂ�̂ŁA1�s���Ȃ��ꍇ�͏o�͂��Ȃ�
                WriteUTF8File(setting.getTmpSRTFilePath(key, lang), srt);
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
    }
    ctx.infoF("�����t�@�C����������: %.2f�b", sw.getAndReset());

    if (setting.isEncodeAudio()) {
        ctx.info("[�����G���R�[�h]");
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
        }
    } else if (setting.getFormat() != FORMAT_TSREPLACE) { // tsreplace�̏ꍇ�͉����t�@�C�������Ȃ�
        ctx.info("[�����o��]");
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
                        // �f���A�����m��2��AAC�ɕ���
                        ctx.infoF("����%d-%d�̓f���A�����m�Ȃ̂�2��AAC�t�@�C���ɕ������܂�", fileIn.outKey.format, asrc);
                        SpDualMonoSplitter splitter(ctx);
                        const tstring filepath0 = setting.getIntAudioFilePath(key, adst++, setting.getAudioEncoder());
                        const tstring filepath1 = setting.getIntAudioFilePath(key, adst++, setting.getAudioEncoder());
                        splitter.open(0, filepath0);
                        splitter.open(1, filepath1);
                        for (int frameIndex : frameList) {
                            splitter.inputPacket(audioCache[frameIndex]);
                        }
                    } else {
                        if (isDualMono) {
                            ctx.infoF("����%d-%d�̓f���A�����m�ł����A�����t�H�[�}�b�g�����w�肪����̂ŕ������܂���", fileIn.outKey.format, asrc);
                        }
                        const tstring filepath = setting.getIntAudioFilePath(key, adst++, setting.getAudioEncoder());
                        File file(filepath, _T("wb"));
                        for (int frameIndex : frameList) {
                            file.write(audioCache[frameIndex]);
                        }
                    }
                }
            }
        }
    }

    auto argGen = std::unique_ptr<EncoderArgumentGenerator>(new EncoderArgumentGenerator(setting, reformInfo));

    sw.start();
    for (int i = 0; i < (int)keys.size(); ++i) {
        auto key = keys[i];
        auto& fileOut = outFileInfo[i];
        const CMAnalyze* cma = cmanalyze[key.video].get();

        AMTFilterSource filterSource(ctx, setting, reformInfo,
            cma->getZones(), cma->getLogoPath(), key, rm);

        if (!setting.getPreEncBatchFile().empty()) {
            ctx.infoF("[�G���R�[�h�O�o�b�`�t�@�C��] %d/%d", i + 1, (int)keys.size());
            ctx.infoF("%s", setting.getPreEncBatchFile().c_str());
            if (executeBatchFile(setting.getPreEncBatchFile(), filterSource.getFormat(), setting, key, serviceId)) {
                THROW(RuntimeException, "�G���R�[�h�O�o�b�`�t�@�C���̎��s�Ɏ��s���܂���");
            }
        }

        try {
            PClip filterClip = filterSource.getClip();
            IScriptEnvironment2* env = filterSource.getEnv();
            auto encoderZones = filterSource.getZones();
            auto& outfmt = filterSource.getFormat();
            auto& outvi = filterClip->GetVideoInfo();
            auto& timeCodes = filterSource.getTimeCodes();

            ctx.infoF("[�G���R�[�h�J�n] %d/%d %s", i + 1, (int)keys.size(), CMTypeToString(key.cm));
            auto bitrate = argGen->printBitrate(ctx, key);

            fileOut.vfmt = outfmt;
            fileOut.srcBitrate = bitrate.first;
            fileOut.targetBitrate = bitrate.second;
            fileOut.vfrTimingFps = filterSource.getVfrTimingFps();

            if (timeCodes.size() > 0) {
                // �t�B���^�ɂ��VFR���L��
                if (eoInfo.afsTimecode) {
                    THROW(ArgumentException, "�G���R�[�_�ƃt�B���^�̗�����VFR�^�C���R�[�h���o�͂���Ă��܂��B");
                }
                if (eoInfo.selectEvery > 1) {
                    THROW(ArgumentException, "VFR�ŏo�͂���ꍇ�́A�G���R�[�_�ŊԈ������Ƃ͂ł��܂���");
                } else if (!setting.isFormatVFRSupported()) {
                    THROW(FormatException, "M2TS/TS�o�͂�VFR���T�|�[�g���Ă��܂���");
                }
                ctx.infoF("VFR�^�C�~���O: %d fps", fileOut.vfrTimingFps);
                fileOut.timecode = setting.getAvsTimecodePath(key);
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
            // VFR�t���[���^�C�~���O��120fps��
            std::vector<tstring> encoderArgs;
            for (int i = 0; i < (int)pass.size(); ++i) {
                encoderArgs.push_back(
                    argGen->GenEncoderOptions(
                        outvi.num_frames,
                        outfmt, bitrateZones, vfrBitrateScale,
                        fileOut.timecode, fileOut.vfrTimingFps, key, pass[i], serviceId, eoInfo));
            }
            // x264, x265, SVT-AV1�̂Ƃ���disablePowerThrottoling=true�Ƃ���
            // QSV/NV/VCEEnc�ł̓v���Z�X���Ŏ����I�ɍœK�Ȃ悤�ɐݒ肳��邽�ߕs�v
            const bool disablePowerThrottoling = (setting.getEncoder() == ENCODER_X264 || setting.getEncoder() == ENCODER_X265 || setting.getEncoder() == ENCODER_SVTAV1);
            AMTFilterVideoEncoder encoder(ctx, std::max(4, setting.getNumEncodeBufferFrames()));
            encoder.encode(filterClip, outfmt,
                timeCodes, encoderArgs, disablePowerThrottoling, env);
        } catch (const AvisynthError& avserror) {
            THROWF(AviSynthException, "%s", avserror.msg);
        }
    }
    ctx.infoF("�G���R�[�h����: %.2f�b", sw.getAndReset());

    argGen = nullptr;

    rm.wait(HOST_CMD_Mux);
    sw.start();
    int64_t totalOutSize = 0;
    auto muxer = std::unique_ptr<AMTMuxder>(new AMTMuxder(ctx, setting, reformInfo));
    for (int i = 0; i < (int)keys.size(); ++i) {
        auto key = keys[i];

        ctx.infoF("[Mux�J�n] %d/%d %s", i + 1, (int)keys.size(), CMTypeToString(key.cm));
        muxer->mux(key, eoInfo, nicoOK, outFileInfo[i]);

        totalOutSize += outFileInfo[i].fileSize;
    }
    ctx.infoF("Mux����: %.2f�b", sw.getAndReset());

    muxer = nullptr;
    thSetPowerThrottling->abortThread();

    // �o�͌��ʂ�\��
    reformInfo.printOutputMapping([&](EncodeFileKey key) {
        const auto& file = reformInfo.getEncodeFile(key);
        return setting.getOutFilePath(file.outKey, file.keyMax, getActualOutputFormat(key, reformInfo, setting), eoInfo.format);
        });

    // �o�͌���JSON�o��
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
        ctx.warn("��ʃt�@�C�����[�h�ł�TS�t�@�C���̏����͔񐄏��ł�");
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

    // �o�͌��ʂ�\��
    ctx.info("����");
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

// TsSplitter���z�֐� //

/* virtual */ void DrcsSearchSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    // ���̏��ŏ��̃t���[�������K�v�Ȃ�����
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
    // �t�@�C���擪����10%�̂Ƃ��납��ǂ�
    srcfile.seek(fileSize / 10, SEEK_SET);
    int64_t totalRead = 0;
    // �Ō��10%�͓ǂ܂Ȃ�
    int64_t end = fileSize / 10 * 9;
    size_t readBytes;
    do {
        readBytes = srcfile.read(buffer);
        inputTsData(MemoryChunk(buffer.data, readBytes));
        totalRead += readBytes;
    } while (totalRead < end && !hasSubtltle_ && videoFrameList_.size() < maxframes);
}

bool SubtitleDetectorSplitter::getHasSubtitle() const {
    return hasSubtltle_;
}

// TsSplitter���z�֐� //

/* virtual */ void SubtitleDetectorSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    // ���̏��ŏ��̃t���[�������K�v�Ȃ�����
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
    // �t�@�C���擪����10%�̂Ƃ��납��ǂ�
    srcfile.seek(fileSize / 10, SEEK_SET);
    int64_t totalRead = 0;
    // �Ō��10%�͓ǂ܂Ȃ�
    int64_t end = fileSize / 10 * 9;
    size_t readBytes;
    do {
        readBytes = srcfile.read(buffer);
        inputTsData(MemoryChunk(buffer.data, readBytes));
        totalRead += readBytes;
    } while (totalRead < end && videoFrameList_.size() < maxframes);
}

// TsSplitter���z�֐� //

/* virtual */ void AudioDetectorSplitter::onVideoPesPacket(
    int64_t clock,
    const std::vector<VideoFrameInfo>& frames,
    PESPacket packet) {
    // ���̏��ŏ��̃t���[�������K�v�Ȃ�����
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
    printf("�C���f�b�N�X: %d �`�����l��: %s �T���v�����[�g: %d\n",
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
    ctx.infoF("����: %.2f�b", sw.getAndReset());
}

/* static */ void detectSubtitleMain(AMTContext& ctx, const ConfigWrapper& setting) {
    auto splitter = std::unique_ptr<SubtitleDetectorSplitter>(new SubtitleDetectorSplitter(ctx, setting));
    if (setting.getServiceId() > 0) {
        splitter->setServiceId(setting.getServiceId());
    }
    splitter->readAll(setting.getMaxFrames());
    printf("����%s\n", splitter->getHasSubtitle() ? "����" : "�Ȃ�");
}

/* static */ void detectAudioMain(AMTContext& ctx, const ConfigWrapper& setting) {
    auto splitter = std::unique_ptr<AudioDetectorSplitter>(new AudioDetectorSplitter(ctx, setting));
    if (setting.getServiceId() > 0) {
        splitter->setServiceId(setting.getServiceId());
    }
    splitter->readAll(setting.getMaxFrames());
}
