#pragma once

/**
* Amtasukaze Transcode Setting
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <string>

#include "StreamUtils.h"

struct EncoderOptionInfo;

struct EncoderZone {
    int startFrame;
    int endFrame;
};

struct BitrateZone : EncoderZone {
    double bitrate;
    double qualityOffset;

    BitrateZone();
    BitrateZone(EncoderZone zone);
    BitrateZone(EncoderZone zone, double bitrate, double qualityOffset);
};

namespace av {

// カラースペース3セット
// x265は数値そのままでもOKだが、x264はhelpを見る限りstringでなければ
// ならないようなので変換を定義
// とりあえずARIB STD-B32 v3.7に書いてあるのだけ

// 3原色
const char* getColorPrimStr(int color_prim);

// ガンマ
const char* getTransferCharacteristicsStr(int transfer_characteritics, bool forSVTAV1);

// 変換係数
const char* getColorSpaceStr(int color_space, bool forSVTAV1);

} // namespace av {

enum ENUM_ENCODER {
    ENCODER_X264,
    ENCODER_X265,
    ENCODER_QSVENC,
    ENCODER_NVENC,
    ENCODER_VCEENC,
    ENCODER_SVTAV1,
};

enum ENUM_FORMAT {
    FORMAT_MP4,
    FORMAT_MKV,
    FORMAT_M2TS,
    FORMAT_TS,
    FORMAT_TSREPLACE,
};

struct BitrateSetting {
    double a, b;
    double h264;
    double h265;

    double getTargetBitrate(VIDEO_STREAM_FORMAT format, double srcBitrate) const;
};

const char* encoderToString(ENUM_ENCODER encoder);

bool encoderOutputInContainer(const ENUM_ENCODER encoder, const ENUM_FORMAT format);

tstring makeEncoderArgs(
    ENUM_ENCODER encoder,
    const tstring& binpath,
    const tstring& options,
    const VideoFormat& fmt,
    const tstring& timecodepath,
    int vfrTimingFps,
    const ENUM_FORMAT format,
    const tstring& outpath);

enum ENUM_AUDIO_ENCODER {
    AUDIO_ENCODER_NONE,
    AUDIO_ENCODER_NEROAAC,
    AUDIO_ENCODER_QAAC,
    AUDIO_ENCODER_FDKAAC,
    AUDIO_ENCODER_OPUSENC,
};

const char* audioEncoderToString(ENUM_AUDIO_ENCODER fmt);

tstring makeAudioEncoderArgs(
    ENUM_AUDIO_ENCODER encoder,
    const tstring& binpath,
    const tstring& options,
    int kbps,
    const tstring& outpath);

std::vector<std::pair<tstring, bool>> makeMuxerArgs(
    const ENUM_ENCODER encoder,
    const std::pair<int, int>& userSAR,
    const ENUM_FORMAT format,
    const tstring& binpath,
    const tstring& timelineeditorpath,
    const tstring& mp4boxpath,
    const tstring& srcTSFilePath,
    const tstring& inVideo,
    const bool encoderOutputInContainer,
    const VideoFormat& videoFormat,
    const std::vector<tstring>& inAudios,
    const tstring& tmpdir,
    const tstring& outpath,
    const tstring& tmpout1path,
    const tstring& tmpout2path,
    const tstring& chapterpath,
    const tstring& timecodepath,
    std::pair<int, int> timebase,
    const std::vector<tstring>& inSubs,
    const std::vector<tstring>& subsTitles,
    const tstring& metapath,
    const bool tsreplaceRemoveTypeD);

tstring makeTimelineEditorArgs(
    const tstring& binpath,
    const tstring& inpath,
    const tstring& outpath,
    const tstring& timecodepath);

const char* cmOutMaskToString(int outmask);

enum AMT_CLI_MODE {
    AMT_CLI_TS,
    AMT_CLI_GENERIC,
};

enum AMT_PRINT_PREFIX {
    AMT_PREFIX_DEFAULT,
    AMT_PREFIX_TIME,
};

class TempDirectory : AMTObject, NonCopyable {
public:
    TempDirectory(AMTContext& ctx, const tstring& tmpdir, bool noRemoveTmp);
    ~TempDirectory();

    void Initialize();

    tstring path() const;

private:
    tstring path_;
    bool initialized_;
    bool noRemoveTmp_;

    tstring genPath(const tstring& base, int code);
};

const char* GetCMSuffix(CMType cmtype);

const char* GetNicoJKSuffix(NicoJKType type);

struct Config {
    // 一時フォルダ
    tstring workDir;
    tstring mode;
    tstring modeArgs; // テスト用
    // 入力ファイルパス（拡張子を含む）
    tstring srcFilePath;
    // 入力ファイルパス(オリジナル)（拡張子を含む）
    tstring srcFilePathOrg;
    // 出力ファイルパス（拡張子を除く）
    tstring outVideoPath;
    // 結果情報JSON出力パス
    tstring outInfoJsonPath;
    // DRCSマッピングファイルパス
    tstring drcsMapPath;
    tstring drcsOutPath;
    // フィルタパス
    tstring filterScriptPath;
    tstring postFilterScriptPath;
    // エンコーダ設定
    ENUM_ENCODER encoder;
    tstring encoderPath;
    tstring encoderOptions;
    std::pair<int, int> userSAR;
    ENUM_AUDIO_ENCODER audioEncoder;
    tstring audioEncoderPath;
    tstring audioEncoderOptions;
    tstring muxerPath;
    tstring timelineditorPath;
    tstring mp4boxPath;
    tstring mkvmergePath;
    tstring nicoConvAssPath;
    tstring nicoConvChSidPath;
    tstring preEncBatchFile;
    ENUM_FORMAT format;
    bool tsreplaceRemoveTypeD;
    bool useMKVWhenSubExist;
    bool splitSub;
    bool twoPass;
    bool autoBitrate;
    bool chapter;
    bool subtitles;
    int nicojkmask;
    bool nicojk18;
    bool useNicoJKLog;
    BitrateSetting bitrate;
    double bitrateCM;
    double cmQualityOffset;
    double x265TimeFactor;
    int serviceId;
    DecoderSetting decoderSetting;
    int audioBitrateInKbps;
    int numEncodeBufferFrames;
    // CM解析用設定
    std::vector<tstring> logoPath;
    std::vector<tstring> eraseLogoPath;
    bool ignoreNoLogo;
    bool ignoreNoDrcsMap;
    bool ignoreNicoJKError;
    double pmtCutSideRate[2];
    bool looseLogoDetection;
    bool noDelogo;
    bool parallelLogoAnalysis;
    int maxFadeLength;
    tstring chapterExePath;
    tstring chapterExeOptions;
    tstring joinLogoScpPath;
    tstring joinLogoScpCmdPath;
    tstring joinLogoScpOptions;
    int cmoutmask;
    tstring trimavsPath;
    // 検出モード用
    int maxframes;
    // ホストプロセスとの通信用
    HANDLE inPipe;
    HANDLE outPipe;
    int affinityGroup;
    uint64_t affinityMask;
    // デバッグ用設定
    bool dumpStreamInfo;
    bool systemAvsPlugin;
    bool noRemoveTmp;
    bool dumpFilter;
    AMT_PRINT_PREFIX printPrefix;
};

class ConfigWrapper : public AMTObject {
public:
    ConfigWrapper(
        AMTContext& ctx,
        const Config& conf);

    tstring getMode() const;

    tstring getModeArgs() const;

    tstring getSrcFilePath() const;

    tstring getSrcFileOriginalPath() const;

    tstring getOutInfoJsonPath() const;

    tstring getFilterScriptPath() const;

    tstring getPostFilterScriptPath() const;

    ENUM_ENCODER getEncoder() const;

    tstring getEncoderPath() const;

    tstring getEncoderOptions() const;

    std::pair<int, int> getUserSAR() const;

    ENUM_AUDIO_ENCODER getAudioEncoder() const;

    bool isEncodeAudio() const;

    tstring getAudioEncoderPath() const;

    tstring getAudioEncoderOptions() const;

    tstring getPreEncBatchFile() const;

    ENUM_FORMAT getFormat() const;

    bool getTsreplaceRemoveTypeD() const;

    bool getUseMKVWhenSubExist() const;

    bool isFormatVFRSupported() const;

    tstring getMuxerPath() const;

    tstring getTimelineEditorPath() const;

    tstring getMp4BoxPath() const;

    tstring getMkvMergePath() const;

    tstring getNicoConvAssPath() const;

    tstring getNicoConvChSidPath() const;

    bool isSplitSub() const;

    bool isTwoPass() const;

    bool isAutoBitrate() const;

    bool isChapterEnabled() const;

    bool isSubtitlesEnabled() const;

    bool isNicoJKEnabled() const;

    bool isNicoJK18Enabled() const;

    bool isUseNicoJKLog() const;

    int getNicoJKMask() const;

    BitrateSetting getBitrate() const;

    double getBitrateCM() const;

    double getCMQualityOffset() const;

    double getX265TimeFactor() const;

    int getServiceId() const;

    DecoderSetting getDecoderSetting() const;

    int getAudioBitrateInKbps() const;

    int getNumEncodeBufferFrames() const;

    const std::vector<tstring>& getLogoPath() const;

    const std::vector<tstring>& getEraseLogoPath() const;

    bool isIgnoreNoLogo() const;

    bool isIgnoreNoDrcsMap() const;

    bool isIgnoreNicoJKError() const;

    bool isPmtCutEnabled() const;

    const double* getPmtCutSideRate() const;

    bool isLooseLogoDetection() const;

    bool isNoDelogo() const;

    bool isParallelLogoAnalysis() const;

    int getMaxFadeLength() const;

    tstring getChapterExePath() const;

    tstring getChapterExeOptions() const;

    tstring getJoinLogoScpPath() const;

    tstring getJoinLogoScpCmdPath() const;

    tstring getJoinLogoScpOptions() const;

    tstring getTrimAVSPath() const;

    const std::vector<CMType>& getCMTypes() const;

    const std::vector<NicoJKType>& getNicoJKTypes() const;

    int getMaxFrames() const;

    HANDLE getInPipe() const;

    HANDLE getOutPipe() const;

    int getAffinityGroup() const;

    uint64_t getAffinityMask() const;

    bool isDumpStreamInfo() const;

    bool isSystemAvsPlugin() const;

    AMT_PRINT_PREFIX getPrintPrefix() const;

    tstring getTmpDir() const;

    tstring getAudioFilePath() const;

    tstring getWaveFilePath() const;

    tstring getIntVideoFilePath(int index) const;

    tstring getStreamInfoPath() const;

    tstring getEncVideoFilePath(EncodeFileKey key) const;

    tstring getEncVideoOptionFilePath(EncodeFileKey key) const;

    tstring getAfsTimecodePath(EncodeFileKey key) const;

    tstring getAvsTmpPath(EncodeFileKey key) const;

    tstring getAvsDurationPath(EncodeFileKey key) const;

    tstring getAvsTimecodePath(EncodeFileKey key) const;

    tstring getFilterAvsPath(EncodeFileKey key) const;

    tstring getEncStatsFilePath(EncodeFileKey key) const;

    tstring getIntAudioFilePath(EncodeFileKey key, int aindex, ENUM_AUDIO_ENCODER encoder) const;

    tstring getTmpASSFilePath(EncodeFileKey key, int langindex) const;

    tstring getTmpSRTFilePath(EncodeFileKey key, int langindex) const;

    tstring getTmpAMTSourcePath(int vindex) const;

    tstring getTmpSourceAVSPath(int vindex) const;

    tstring getTmpSourceAVS8bitPath(int vindex) const;

    tstring getTmpLogoFramePath(int vindex, int logoIndex = -1) const;

    tstring getTmpChapterExePath(int vindex) const;

    tstring getTmpChapterExeOutPath(int vindex) const;

    tstring getTmpTrimAVSPath(int vindex) const;

    tstring getTmpJlsPath(int vindex) const;

    tstring getTmpDivPath(int vindex) const;

    tstring getTmpChapterPath(EncodeFileKey key) const;

    tstring getTmpNicoJKXMLPath() const;

    tstring getTmpNicoJKASSPath(NicoJKType type) const;

    tstring getTmpNicoJKASSPath(EncodeFileKey key, NicoJKType type) const;

    tstring getVfrTmpFile1Path(EncodeFileKey key, ENUM_FORMAT format) const;

    tstring getVfrTmpFile2Path(EncodeFileKey key, ENUM_FORMAT format) const;

    tstring getM2tsMetaFilePath(EncodeFileKey key) const;

    const tchar* getOutputExtention(ENUM_FORMAT format) const;

    tstring getOutFileBaseWithoutPrefix() const;

    tstring getOutFileBase(EncodeFileKey key, EncodeFileKey keyMax, ENUM_FORMAT format, VIDEO_STREAM_FORMAT codec) const;

    tstring getOutFilePath(EncodeFileKey key, EncodeFileKey keyMax, ENUM_FORMAT format, VIDEO_STREAM_FORMAT codec) const;

    tstring getOutASSPath(EncodeFileKey key, EncodeFileKey keyMax, ENUM_FORMAT format, VIDEO_STREAM_FORMAT codec, int langidx, NicoJKType jktype) const;

    tstring getOutChapterPath(EncodeFileKey key, EncodeFileKey keyMax, ENUM_FORMAT format, VIDEO_STREAM_FORMAT codec) const;

    tstring getOutSummaryPath() const;

    tstring getDRCSMapPath() const;

    tstring getDRCSOutPath(const std::string& md5) const;

    bool isDumpFilter() const;

    tstring getFilterGraphDumpPath(EncodeFileKey key) const;

    bool isZoneAvailable() const;

    bool isZoneWithoutBitrateAvailable() const;

    bool isZoneWithQualityAvailable() const;

    bool isEncoderSupportVFR() const;

    bool isBitrateCMEnabled() const;

    tstring getOptions(
        int numFrames,
        VIDEO_STREAM_FORMAT srcFormat, double srcBitrate, bool pulldown,
        int pass, const std::vector<BitrateZone>& zones, const tstring& optionFilePath, double vfrBitrateScale,
        EncodeFileKey key, const EncoderOptionInfo& eoInfo) const;

    void dump() const;

    void CreateTempDir();

private:
    Config conf;
    TempDirectory tmpDir;
    std::vector<CMType> cmtypes;
    std::vector<NicoJKType> nicojktypes;

    const char* decoderToString(DECODER_TYPE decoder) const;

    const char* formatToString(ENUM_FORMAT fmt) const;

    tstring regtmp(tstring str) const;
};

