/**
* Amtasukaze Avisynth Source Plugin
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "TranscodeSetting.h"
#include "EncoderOptionParser.h"

// �J���[�X�y�[�X��`���g������
#include "libavutil/pixfmt.h"

BitrateZone::BitrateZone() :
    EncoderZone(),
    bitrate(0.0),
    qualityOffset(0.0) {}
BitrateZone::BitrateZone(EncoderZone zone) :
    EncoderZone(zone),
    bitrate(0.0),
    qualityOffset(0.0) {}
BitrateZone::BitrateZone(EncoderZone zone, double bitrate, double qualityOffset) :
    EncoderZone(zone),
    bitrate(bitrate),
    qualityOffset(qualityOffset) {}

// �J���[�X�y�[�X3�Z�b�g
// x265�͐��l���̂܂܂ł�OK�����Ax264��help���������string�łȂ����
// �Ȃ�Ȃ��悤�Ȃ̂ŕϊ����`
// �Ƃ肠����ARIB STD-B32 v3.7�ɏ����Ă���̂���

// 3���F
/* static */ const char* av::getColorPrimStr(int color_prim) {
    switch (color_prim) {
    case AVCOL_PRI_BT709: return "bt709";
    case AVCOL_PRI_BT2020: return "bt2020";
    default:
        THROWF(FormatException,
            "Unsupported color primaries (%d)", color_prim);
    }
    return NULL;
}

// �K���}
/* static */ const char* av::getTransferCharacteristicsStr(int transfer_characteritics, bool forSVTAV1) {
    switch (transfer_characteritics) {
    case AVCOL_TRC_BT709: return "bt709";
    case AVCOL_TRC_IEC61966_2_4: return (forSVTAV1) ? "iec61966" : "iec61966-2-4";
    case AVCOL_TRC_BT2020_10: return "bt2020-10";
    case AVCOL_TRC_SMPTEST2084: return (forSVTAV1) ? "smpte2084" : "smpte-st-2084";
    case AVCOL_TRC_ARIB_STD_B67: return (forSVTAV1) ? "hlg" : "arib-std-b67";
    default:
        THROWF(FormatException,
            "Unsupported color transfer characteritics (%d)", transfer_characteritics);
    }
    return NULL;
}

// �ϊ��W��
/* static */ const char* av::getColorSpaceStr(int color_space, bool forSVTAV1) {
    switch (color_space) {
    case AVCOL_SPC_BT709: return "bt709";
    case AVCOL_SPC_BT2020_NCL: return (forSVTAV1) ? "bt2020-ncl" : "bt2020nc";
    default:
        THROWF(FormatException,
            "Unsupported color color space (%d)", color_space);
    }
    return NULL;
}

double BitrateSetting::getTargetBitrate(VIDEO_STREAM_FORMAT format, double srcBitrate) const {
    double base = a * srcBitrate + b;
    if (format == VS_H264) {
        return base * h264;
    } else if (format == VS_H265) {
        return base * h265;
    }
    return base;
}

/* static */ const char* encoderToString(ENUM_ENCODER encoder) {
    switch (encoder) {
    case ENCODER_X264: return "x264";
    case ENCODER_X265: return "x265";
    case ENCODER_QSVENC: return "QSVEnc";
    case ENCODER_NVENC: return "NVEnc";
    case ENCODER_VCEENC: return "VCEEnc";
    case ENCODER_SVTAV1: return "SVT-AV1";
    }
    return "Unknown";
}

/* static */ bool encoderOutputInContainer(const ENUM_ENCODER encoder, const ENUM_FORMAT format) {
    switch (encoder) {
    case ENCODER_QSVENC:
    case ENCODER_NVENC:
    case ENCODER_VCEENC:
        return (format == FORMAT_MP4 || format == FORMAT_MKV || format == FORMAT_TSREPLACE);
    default:
        break;
    }
    return false;
}

/* static */ tstring makeEncoderArgs(
    ENUM_ENCODER encoder,
    const tstring& binpath,
    const tstring& options,
    const VideoFormat& fmt,
    const tstring& timecodepath,
    int vfrTimingFps,
    const ENUM_FORMAT format,
    const tstring& outpath) {
    StringBuilderT sb;

    sb.append(_T("\"%s\""), binpath);

    // y4m�w�b�_�ɂ���̂ŕK�v�Ȃ�
    //ss << " --fps " << fmt.frameRateNum << "/" << fmt.frameRateDenom;
    //ss << " --input-res " << fmt.width << "x" << fmt.height;
    //ss << " --sar " << fmt.sarWidth << ":" << fmt.sarHeight;

    if (encoder == ENCODER_SVTAV1) {
        if (fmt.colorPrimaries != AVCOL_PRI_UNSPECIFIED) {
            sb.append(_T(" --color-primaries %s"), av::getColorPrimStr(fmt.colorPrimaries));
        }
        if (fmt.transferCharacteristics != AVCOL_TRC_UNSPECIFIED) {
            sb.append(_T(" --transfer-characteristics %s"), av::getTransferCharacteristicsStr(fmt.transferCharacteristics, true));
        }
        if (fmt.colorSpace != AVCOL_TRC_UNSPECIFIED) {
            sb.append(_T(" --matrix-coefficients %s"), av::getColorSpaceStr(fmt.colorSpace, true));
        }
    } else {
        if (fmt.colorPrimaries != AVCOL_PRI_UNSPECIFIED) {
            sb.append(_T(" --colorprim %s"), av::getColorPrimStr(fmt.colorPrimaries));
        }
        if (fmt.transferCharacteristics != AVCOL_TRC_UNSPECIFIED) {
            sb.append(_T(" --transfer %s"), av::getTransferCharacteristicsStr(fmt.transferCharacteristics, false));
        }
        if (fmt.colorSpace != AVCOL_TRC_UNSPECIFIED) {
            sb.append(_T(" --colormatrix %s"), av::getColorSpaceStr(fmt.colorSpace, false));
        }
    }

    // �C���^�[���[�X
    switch (encoder) {
    case ENCODER_X264:
    case ENCODER_QSVENC:
    case ENCODER_NVENC:
    case ENCODER_VCEENC:
        sb.append(fmt.progressive ? _T("") : _T(" --tff"));
        break;
    case ENCODER_X265:
        //sb.append(fmt.progressive ? " --no-interlace" : " --interlace tff");
        if (fmt.progressive == false) {
            THROW(ArgumentException, "HEVC�̃C���^�[���[�X�o�͂ɂ͑Ή����Ă��܂���");
        }
        break;
    case ENCODER_SVTAV1:
        if (fmt.progressive == false) {
            THROW(ArgumentException, "AV1�̃C���^�[���[�X�o�͂ɂ͑Ή����Ă��܂���");
        }
        break;
    }

    if (encoder == ENCODER_SVTAV1) {
        sb.append(_T(" %s -b \"%s\" --progress 2"), options, outpath);
    } else {
        sb.append(_T(" %s -o \"%s\""), options, outpath);
    }

    // ���͌`��
    switch (encoder) {
    case ENCODER_X264:
        sb.append(_T(" --stitchable"))
            .append(_T(" --demuxer y4m -"));
        break;
    case ENCODER_X265:
        sb.append(_T(" --no-opt-qp-pps --no-opt-ref-list-length-pps"))
            .append(_T(" --y4m --input -"));
        break;
    case ENCODER_QSVENC:
    case ENCODER_NVENC:
    case ENCODER_VCEENC:
        if (encoderOutputInContainer(encoder, format)) {
            if (format == FORMAT_MKV) {
                sb.append(_T(" --output-format matroska"));
            } else if (format == FORMAT_MP4 || format == FORMAT_TSREPLACE) {
                sb.append(_T(" --output-format mp4"));
            }
        }
        sb.append(_T(" --y4m -i -"));
        break;
    case ENCODER_SVTAV1:
        sb.append(_T(" -i stdin"));
        break;
    }

    if (timecodepath.size() > 0
        && (encoder == ENCODER_X264
            || encoder == ENCODER_QSVENC
            || encoder == ENCODER_NVENC
            || encoder == ENCODER_VCEENC)) {
        std::pair<int, int> timebase = std::make_pair(fmt.frameRateNum * (vfrTimingFps / 30), fmt.frameRateDenom);
        sb.append(_T(" --tcfile-in \"%s\" --timebase %d/%d"), timecodepath, timebase.second, timebase.first);
    }

    return sb.str();
}

/* static */ const char* audioEncoderToString(ENUM_AUDIO_ENCODER fmt) {
    switch (fmt) {
    case AUDIO_ENCODER_NONE: return "none";
    case AUDIO_ENCODER_NEROAAC: return "neroaac";
    case AUDIO_ENCODER_QAAC: return "qaac";
    case AUDIO_ENCODER_FDKAAC: return "fdkaac";
    case AUDIO_ENCODER_OPUSENC: return "opus";
    }
    return "unknown";
}

/* static */ tstring makeAudioEncoderArgs(
    ENUM_AUDIO_ENCODER encoder,
    const tstring& binpath,
    const tstring& options,
    int kbps,
    const tstring& outpath) {
    StringBuilderT sb;

    sb.append(_T("\"%s\" %s"), binpath, options);

    if (kbps) {
        switch (encoder) {
        case AUDIO_ENCODER_NEROAAC:
            sb.append(_T(" -br %d "), kbps * 1000);
            break;
        case AUDIO_ENCODER_QAAC:
            sb.append(_T(" -a %d "), kbps * 1000);
            break;
        case AUDIO_ENCODER_FDKAAC:
            sb.append(_T(" -b %d "), kbps * 1000);
            break;
        case AUDIO_ENCODER_OPUSENC:
            sb.append(_T(" --vbr --bitrate %d "), kbps);
            break;
        }
    }

    switch (encoder) {
    case AUDIO_ENCODER_NEROAAC:
        sb.append(_T(" -ignorelength -if - -of \"%s\""), outpath);
        break;
    case AUDIO_ENCODER_QAAC:
    case AUDIO_ENCODER_FDKAAC:
        sb.append(_T(" -o \"%s\" -"), outpath);
        break;
    case AUDIO_ENCODER_OPUSENC:
        sb.append(_T(" - \"%s\""), outpath);
        break;
    }

    return sb.str();
}

bool sarValid(const std::pair<int, int>& sar) {
    return sar.first > 0 && sar.second > 0;
}

/* static */ std::vector<std::pair<tstring, bool>> makeMuxerArgs(
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
    const bool tsreplaceRemoveTypeD) {
    std::vector<std::pair<tstring, bool>> ret;

    StringBuilderT sb;
    sb.append(_T("\"%s\""), binpath);

    if (format == FORMAT_MP4) {
        bool needChapter = (chapterpath.size() > 0);
        bool needSubs = (inSubs.size() > 0);
        const bool needTimecode = (timecodepath.size() > 0);

        sb.clear();
        sb.append(_T("\"%s\""), mp4boxpath);
        sb.append(_T(" -brand mp42 -ab mp41 -ab iso2"));
        sb.append(_T(" -tmp \"%s\""), tmpdir);
        sb.append(_T(" -add \"%s#video:name=Video:forcesync"), inVideo);
        if (!encoderOutputInContainer) {
            if (videoFormat.fixedFrameRate) {
                sb.append(_T(":fps=%d/%d"), videoFormat.frameRateNum, videoFormat.frameRateDenom);
            }
            if (encoder == ENCODER_SVTAV1 && (!videoFormat.isSARUnspecified() || sarValid(userSAR))) {
                const int sarW = sarValid(userSAR) ? userSAR.first : videoFormat.sarWidth;
                const int sarH = sarValid(userSAR) ? userSAR.second : videoFormat.sarHeight;
                sb.append(_T(":par=%d:%d"), sarW, sarH);
            }
        }
        sb.append(_T("\""));
        for (int i = 0; i < (int)inAudios.size(); ++i) {
            sb.append(_T(" -add \"%s\"#audio:name=Audio%d"), inAudios[i], i);
        }
        if (needChapter && !needTimecode) {
            sb.append(_T(" -chap \"%s\""), chapterpath);
            needChapter = false;
        }
        if (needSubs && !needTimecode) {
            for (int i = 0; i < (int)inSubs.size(); ++i) {
                if (subsTitles[i] == _T("SRT")) { // mp4��SRT�̂�
                    sb.append(_T(" -add \"%s#:name=%s\""), inSubs[i], subsTitles[i]);
                }
            }
            needSubs = false;
        }
        tstring dst = (needTimecode || needChapter || needSubs) ? tmpout1path : outpath;
        sb.append(_T(" -new \"%s\""), dst);
        ret.push_back(std::make_pair(sb.str(), true));
        sb.clear();

        if (needTimecode) {
            const tstring timelineeditorout = (needChapter || needSubs) ? tmpout2path : outpath;
            // �K�v�Ȃ�timelineeditor��timecode�𖄂ߍ���
            sb.append(_T("\"%s\""), timelineeditorpath)
                .append(_T(" --track 1"))
                .append(_T(" --timecode \"%s\""), timecodepath)
                .append(_T(" --media-timescale %d"), timebase.first)
                .append(_T(" --media-timebase %d"), timebase.second)
                .append(_T(" \"%s\""), dst)
                .append(_T(" \"%s\""), timelineeditorout);
            ret.push_back(std::make_pair(sb.str(), false));
            sb.clear();
            dst = timelineeditorout;
        }

        if (needChapter || needSubs) {
            // �����ƃ`���v�^�[�𖄂ߍ���
            sb.append(_T("\"%s\" -brand mp42 -ab mp41 -ab iso2"), mp4boxpath);
            sb.append(_T(" -add \"%s\""), dst);
            sb.append(_T(" -tmp \"%s\""), tmpdir);
            for (int i = 0; i < (int)inSubs.size(); ++i) {
                if (subsTitles[i] == _T("SRT")) { // mp4��SRT�̂�
                    sb.append(_T(" -add \"%s#:name=%s\""), inSubs[i], subsTitles[i]);
                }
            }
            // timelineeditor���`���v�^�[�������̂�timecode�����鎞��mp4box�œ����
            // timecode������ꍇ�͂������Ń`���v�^�[������
            if (needChapter) {
                sb.append(_T(" -chap \"%s\""), chapterpath);
            }
            sb.append(_T(" -new \"%s\""), outpath);
            ret.push_back(std::make_pair(sb.str(), true));
            sb.clear();
        }
    } else if (format == FORMAT_MKV) {

        if (chapterpath.size() > 0) {
            sb.append(_T(" --chapters \"%s\""), chapterpath);
        }

        sb.append(_T(" -o \"%s\""), outpath);

        if (timecodepath.size()) {
            sb.append(_T(" --timestamps \"0:%s\""), timecodepath);
        } else if (!encoderOutputInContainer) {
            sb.append(_T(" --default-duration \"0:%d/%dfps\""), videoFormat.frameRateNum, videoFormat.frameRateDenom);
        }
        if (!encoderOutputInContainer && encoder == ENCODER_SVTAV1 && (!videoFormat.isSARUnspecified() || sarValid(userSAR))) {
            const int sarW = sarValid(userSAR) ? userSAR.first : videoFormat.sarWidth;
            const int sarH = sarValid(userSAR) ? userSAR.second : videoFormat.sarHeight;
            int x = videoFormat.width * sarW;
            int y = videoFormat.height * sarH;
            int a = x, b = y, c;
            while ((c = a % b) != 0)
                a = b, b = c;
            x /= b;
            y /= b;
            const double ratio = (sarW >= sarH)
                ? videoFormat.height / (double)y
                : videoFormat.width / (double)x;
            const int disp_w = (int)(x * ratio + 0.5);
            const int disp_h = (int)(y * ratio + 0.5);
            sb.append(_T(" --display-dimensions \"0:%dx%d\""), disp_w, disp_h);
        }
        sb.append(_T(" \"%s\""), inVideo);

        for (const auto& inAudio : inAudios) {
            sb.append(_T(" \"%s\""), inAudio);
        }
        for (int i = 0; i < (int)inSubs.size(); ++i) {
            sb.append(_T(" --track-name \"0:%s\" \"%s\""), subsTitles[i], inSubs[i]);
        }

        ret.push_back(std::make_pair(sb.str(), true));
        sb.clear();
    } else if (format == FORMAT_TSREPLACE) {
        tstring tmppath = inVideo;
        if (!encoderOutputInContainer) {
            const bool needTimecode = (timecodepath.size() > 0);

            sb.clear();
            sb.append(_T("\"%s\""), mp4boxpath);
            sb.append(_T(" -brand mp42 -ab mp41 -ab iso2"));
            sb.append(_T(" -add \"%s#video:name=Video:forcesync"), inVideo);
            if (!encoderOutputInContainer) {
                if (videoFormat.fixedFrameRate) {
                    sb.append(_T(":fps=%d/%d"), videoFormat.frameRateNum, videoFormat.frameRateDenom);
                }
                //if (encoder == ENCODER_SVTAV1 && (!videoFormat.isSARUnspecified() || sarValid(userSAR))) {
                //    const int sarW = sarValid(userSAR) ? userSAR.first : videoFormat.sarWidth;
                //    const int sarH = sarValid(userSAR) ? userSAR.second : videoFormat.sarHeight;
                //    sb.append(_T(":par=%d:%d"), sarW, sarH);
                //}
            }
            sb.append(_T("\""));
            sb.append(_T(" -new \"%s\""), tmpout1path);
            ret.push_back(std::make_pair(sb.str(), true));
            sb.clear();
            tmppath = tmpout1path;

            if (needTimecode) {
                const tstring timelineeditorout = tmpout2path;
                // �K�v�Ȃ�timelineeditor��timecode�𖄂ߍ���
                sb.append(_T("\"%s\""), timelineeditorpath)
                    .append(_T(" --track 1"))
                    .append(_T(" --timecode \"%s\""), timecodepath)
                    .append(_T(" --media-timescale %d"), timebase.first)
                    .append(_T(" --media-timebase %d"), timebase.second)
                    .append(_T(" \"%s\""), tmppath)
                    .append(_T(" \"%s\""), timelineeditorout);
                ret.push_back(std::make_pair(sb.str(), false));
                sb.clear();
                tmppath = timelineeditorout;
            }
        }
        sb.clear();
        sb.append(_T("\"%s\""), binpath);
        sb.append(_T(" -i \"%s\""), srcTSFilePath);
        sb.append(_T(" -r \"%s\""), tmppath);
        sb.append(_T(" --replace-format mp4"));
        if (tsreplaceRemoveTypeD) {
            sb.append(_T(" --remove-typed"));
        }
        sb.append(_T(" -o \"%s\""), outpath);
        ret.push_back(std::make_pair(sb.str(), true));
        sb.clear();
    } else { // M2TS or TS
        sb.append(_T(" \"%s\" \"%s\""), metapath, outpath);
        ret.push_back(std::make_pair(sb.str(), true));
        sb.clear();
    }

    return ret;
}

/* static */ tstring makeTimelineEditorArgs(
    const tstring& binpath,
    const tstring& inpath,
    const tstring& outpath,
    const tstring& timecodepath) {
    StringBuilderT sb;
    sb.append(_T("\"%s\""), binpath)
        .append(_T(" --track 1"))
        .append(_T(" --timecode \"%s\""), timecodepath)
        .append(_T(" \"%s\""), inpath)
        .append(_T(" \"%s\""), outpath);
    return sb.str();
}

/* static */ const char* cmOutMaskToString(int outmask) {
    switch (outmask) {
    case 1: return "�ʏ�";
    case 2: return "CM���J�b�g";
    case 3: return "�ʏ�o�͂�CM�J�b�g�o��";
    case 4: return "CM�̂�";
    case 5: return "�ʏ�o�͂�CM�o��";
    case 6: return "�{�҂�CM�𕪗�";
    case 7: return "�ʏ�,�{��,CM�S�o��";
    }
    return "�s��";
}
TempDirectory::TempDirectory(AMTContext& ctx, const tstring& tmpdir, bool noRemoveTmp)
    : AMTObject(ctx)
    , path_(tmpdir)
    , initialized_(false)
    , noRemoveTmp_(noRemoveTmp) {}
TempDirectory::~TempDirectory() {
    if (!initialized_ || noRemoveTmp_) {
        return;
    }
    // �ꎞ�t�@�C�����폜
    ctx.clearTmpFiles();
    // �f�B���N�g���폜
    if (rmdirT(path_.c_str()) != 0) {
        ctx.warnF("�ꎞ�f�B���N�g���폜�Ɏ��s: ", path_);
    }
}

void TempDirectory::Initialize() {
    if (initialized_) return;

    for (int code = (int)time(NULL) & 0xFFFFFF; code > 0; ++code) {
        auto path = genPath(path_, code);
        if (mkdirT(path.c_str()) == 0) {
            path_ = path;
            break;
        }
        if (errno != EEXIST) {
            break;
        }
    }
    if (path_.size() == 0) {
        THROW(IOException, "�ꎞ�f�B���N�g���쐬���s");
    }

    tstring abolutePath;
    int sz = GetFullPathNameW(path_.c_str(), 0, 0, 0);
    abolutePath.resize(sz);
    GetFullPathNameW(path_.c_str(), sz, &abolutePath[0], 0);
    abolutePath.resize(sz - 1);
    path_ = pathNormalize(abolutePath);
    initialized_ = true;
}

tstring TempDirectory::path() const {
    if (!initialized_) {
        THROW(InvalidOperationException, "�ꎞ�f�B���N�g�����쐬���Ă��܂���");
    }
    return path_;
}

tstring TempDirectory::genPath(const tstring& base, int code) {
    return StringFormat(_T("%s/amt%d"), base, code);
}

/* static */ const char* GetCMSuffix(CMType cmtype) {
    switch (cmtype) {
    case CMTYPE_CM: return "-cm";
    case CMTYPE_NONCM: return "-main";
    case CMTYPE_BOTH: return "";
    }
    return "";
}

/* static */ const char* GetNicoJKSuffix(NicoJKType type) {
    switch (type) {
    case NICOJK_720S: return "-720S";
    case NICOJK_720T: return "-720T";
    case NICOJK_1080S: return "-1080S";
    case NICOJK_1080T: return "-1080T";
    }
    return "";
}
ConfigWrapper::ConfigWrapper(
    AMTContext& ctx,
    const Config& conf)
    : AMTObject(ctx)
    , conf(conf)
    , tmpDir(ctx, conf.workDir, conf.noRemoveTmp) {
    for (int cmtypei = 0; cmtypei < CMTYPE_MAX; ++cmtypei) {
        if (conf.cmoutmask & (1 << cmtypei)) {
            cmtypes.push_back((CMType)cmtypei);
        }
    }
    for (int nicotypei = 0; nicotypei < NICOJK_MAX; ++nicotypei) {
        if (conf.nicojkmask & (1 << nicotypei)) {
            nicojktypes.push_back((NicoJKType)nicotypei);
        }
    }
}

tstring ConfigWrapper::getMode() const {
    return conf.mode;
}

tstring ConfigWrapper::getModeArgs() const {
    return conf.modeArgs;
}

tstring ConfigWrapper::getSrcFilePath() const {
    return conf.srcFilePath;
}

tstring ConfigWrapper::getSrcFileOriginalPath() const {
    return conf.srcFilePathOrg;
}

tstring ConfigWrapper::getOutInfoJsonPath() const {
    return conf.outInfoJsonPath;
}

tstring ConfigWrapper::getFilterScriptPath() const {
    return conf.filterScriptPath;
}

tstring ConfigWrapper::getPostFilterScriptPath() const {
    return conf.postFilterScriptPath;
}

ENUM_ENCODER ConfigWrapper::getEncoder() const {
    return conf.encoder;
}

tstring ConfigWrapper::getEncoderPath() const {
    return conf.encoderPath;
}

tstring ConfigWrapper::getEncoderOptions() const {
    return conf.encoderOptions;
}

std::pair<int, int> ConfigWrapper::getUserSAR() const {
    return conf.userSAR;
}

ENUM_AUDIO_ENCODER ConfigWrapper::getAudioEncoder() const {
    return conf.audioEncoder;
}

bool ConfigWrapper::isEncodeAudio() const {
    return conf.audioEncoder != AUDIO_ENCODER_NONE;
}

tstring ConfigWrapper::getAudioEncoderPath() const {
    return conf.audioEncoderPath;
}

tstring ConfigWrapper::getAudioEncoderOptions() const {
    return conf.audioEncoderOptions;
}

ENUM_FORMAT ConfigWrapper::getFormat() const {
    return conf.format;
}

bool ConfigWrapper::getTsreplaceRemoveTypeD() const {
    return conf.tsreplaceRemoveTypeD;
}

bool ConfigWrapper::getUseMKVWhenSubExist() const {
    return conf.useMKVWhenSubExist;
}

bool ConfigWrapper::isFormatVFRSupported() const {
    return conf.format != FORMAT_M2TS && conf.format != FORMAT_TS;
}

tstring ConfigWrapper::getMuxerPath() const {
    return conf.muxerPath;
}

tstring ConfigWrapper::getTimelineEditorPath() const {
    return conf.timelineditorPath;
}

tstring ConfigWrapper::getMp4BoxPath() const {
    return conf.mp4boxPath;
}

tstring ConfigWrapper::getMkvMergePath() const {
    return conf.mkvmergePath;
}

tstring ConfigWrapper::getNicoConvAssPath() const {
    return conf.nicoConvAssPath;
}

tstring ConfigWrapper::getNicoConvChSidPath() const {
    return conf.nicoConvChSidPath;
}

bool ConfigWrapper::isSplitSub() const {
    return conf.splitSub;
}

bool ConfigWrapper::isTwoPass() const {
    return conf.twoPass;
}

bool ConfigWrapper::isAutoBitrate() const {
    return conf.autoBitrate;
}

bool ConfigWrapper::isChapterEnabled() const {
    return conf.chapter;
}

bool ConfigWrapper::isSubtitlesEnabled() const {
    return conf.subtitles;
}

bool ConfigWrapper::isNicoJKEnabled() const {
    return conf.nicojkmask != 0;
}

bool ConfigWrapper::isNicoJK18Enabled() const {
    return conf.nicojk18;
}

bool ConfigWrapper::isUseNicoJKLog() const {
    return conf.useNicoJKLog;
}

int ConfigWrapper::getNicoJKMask() const {
    return conf.nicojkmask;
}

BitrateSetting ConfigWrapper::getBitrate() const {
    return conf.bitrate;
}

double ConfigWrapper::getBitrateCM() const {
    return conf.bitrateCM;
}

double ConfigWrapper::getCMQualityOffset() const {
    return conf.cmQualityOffset;
}

double ConfigWrapper::getX265TimeFactor() const {
    return conf.x265TimeFactor;
}

int ConfigWrapper::getServiceId() const {
    return conf.serviceId;
}

DecoderSetting ConfigWrapper::getDecoderSetting() const {
    return conf.decoderSetting;
}

int ConfigWrapper::getAudioBitrateInKbps() const {
    return conf.audioBitrateInKbps;
}

int ConfigWrapper::getNumEncodeBufferFrames() const {
    return conf.numEncodeBufferFrames;
}

const std::vector<tstring>& ConfigWrapper::getLogoPath() const {
    return conf.logoPath;
}

const std::vector<tstring>& ConfigWrapper::getEraseLogoPath() const {
    return conf.eraseLogoPath;
}

bool ConfigWrapper::isIgnoreNoLogo() const {
    return conf.ignoreNoLogo;
}

bool ConfigWrapper::isIgnoreNoDrcsMap() const {
    return conf.ignoreNoDrcsMap;
}

bool ConfigWrapper::isIgnoreNicoJKError() const {
    return conf.ignoreNicoJKError;
}

bool ConfigWrapper::isPmtCutEnabled() const {
    return conf.pmtCutSideRate[0] > 0 || conf.pmtCutSideRate[1] > 0;
}

const double* ConfigWrapper::getPmtCutSideRate() const {
    return conf.pmtCutSideRate;
}

bool ConfigWrapper::isLooseLogoDetection() const {
    return conf.looseLogoDetection;
}

bool ConfigWrapper::isNoDelogo() const {
    return conf.noDelogo;
}

bool ConfigWrapper::isParallelLogoAnalysis() const {
    return conf.parallelLogoAnalysis;
}

int ConfigWrapper::getMaxFadeLength() const {
    return conf.maxFadeLength;
}

tstring ConfigWrapper::getChapterExePath() const {
    return conf.chapterExePath;
}

tstring ConfigWrapper::getChapterExeOptions() const {
    return conf.chapterExeOptions;
}

tstring ConfigWrapper::getJoinLogoScpPath() const {
    return conf.joinLogoScpPath;
}

tstring ConfigWrapper::getJoinLogoScpCmdPath() const {
    return conf.joinLogoScpCmdPath;
}

tstring ConfigWrapper::getJoinLogoScpOptions() const {
    return conf.joinLogoScpOptions;
}

tstring ConfigWrapper::getTrimAVSPath() const {
    return conf.trimavsPath;
}

const std::vector<CMType>& ConfigWrapper::getCMTypes() const {
    return cmtypes;
}

const std::vector<NicoJKType>& ConfigWrapper::getNicoJKTypes() const {
    return nicojktypes;
}

int ConfigWrapper::getMaxFrames() const {
    return conf.maxframes;
}

HANDLE ConfigWrapper::getInPipe() const {
    return conf.inPipe;
}

HANDLE ConfigWrapper::getOutPipe() const {
    return conf.outPipe;
}

int ConfigWrapper::getAffinityGroup() const {
    return conf.affinityGroup;
}

uint64_t ConfigWrapper::getAffinityMask() const {
    return conf.affinityMask;
}

bool ConfigWrapper::isDumpStreamInfo() const {
    return conf.dumpStreamInfo;
}

bool ConfigWrapper::isSystemAvsPlugin() const {
    return conf.systemAvsPlugin;
}

AMT_PRINT_PREFIX ConfigWrapper::getPrintPrefix() const {
    return conf.printPrefix;
}

tstring ConfigWrapper::getTmpDir() const {
    return tmpDir.path();
}

tstring ConfigWrapper::getAudioFilePath() const {
    return regtmp(StringFormat(_T("%s/audio.dat"), tmpDir.path()));
}

tstring ConfigWrapper::getWaveFilePath() const {
    return regtmp(StringFormat(_T("%s/audio.wav"), tmpDir.path()));
}

tstring ConfigWrapper::getIntVideoFilePath(int index) const {
    return regtmp(StringFormat(_T("%s/i%d.mpg"), tmpDir.path(), index));
}

tstring ConfigWrapper::getStreamInfoPath() const {
    return conf.outVideoPath + _T("-streaminfo.dat");
}

tstring ConfigWrapper::getEncVideoFilePath(EncodeFileKey key) const {
    return regtmp(StringFormat(_T("%s/v%d-%d-%d%s.raw"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm)));
}

tstring ConfigWrapper::getEncVideoOptionFilePath(EncodeFileKey key) const {
    return regtmp(StringFormat(_T("%s/v%d-%d-%d%s.opt.txt"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm)));
}

tstring ConfigWrapper::getAfsTimecodePath(EncodeFileKey key) const {
    return regtmp(StringFormat(_T("%s/v%d-%d-%d%s.timecode.txt"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm)));
}

tstring ConfigWrapper::getAvsTmpPath(EncodeFileKey key) const {
    auto str = StringFormat(_T("%s/v%d-%d-%d%s.avstmp"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm));
    ctx.registerTmpFile(str + _T("*"));
    return str;
}

tstring ConfigWrapper::getAvsDurationPath(EncodeFileKey key) const {
    return regtmp(StringFormat(_T("%s/v%d-%d-%d%s.avstmp"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm)) + _T(".duration.txt"));
}

tstring ConfigWrapper::getAvsTimecodePath(EncodeFileKey key) const {
    return regtmp(StringFormat(_T("%s/v%d-%d-%d%s.avstmp"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm)) + _T(".timecode.txt"));
}

tstring ConfigWrapper::getFilterAvsPath(EncodeFileKey key) const {
    auto str = StringFormat(_T("%s/vfilter%d-%d-%d%s.avs"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm));
    ctx.registerTmpFile(str);
    return str;
}

tstring ConfigWrapper::getEncStatsFilePath(EncodeFileKey key) const {
    auto str = StringFormat(_T("%s/s%d-%d-%d%s.log"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm));
    ctx.registerTmpFile(str);
    // x264��.mbtree����������̂�
    ctx.registerTmpFile(str + _T(".mbtree"));
    // x265��.cutree����������̂�
    ctx.registerTmpFile(str + _T(".cutree"));
    return str;
}

tstring ConfigWrapper::getIntAudioFilePath(EncodeFileKey key, int aindex, ENUM_AUDIO_ENCODER encoder) const {
    return regtmp(StringFormat((encoder == AUDIO_ENCODER_OPUSENC) ? _T("%s/a%d-%d-%d-%d%s.opus") : _T("%s/a%d-%d-%d-%d%s.aac"),
        tmpDir.path(), key.video, key.format, key.div, aindex, GetCMSuffix(key.cm)));
}

tstring ConfigWrapper::getTmpASSFilePath(EncodeFileKey key, int langindex) const {
    return regtmp(StringFormat(_T("%s/c%d-%d-%d-%d%s.ass"),
        tmpDir.path(), key.video, key.format, key.div, langindex, GetCMSuffix(key.cm)));
}

tstring ConfigWrapper::getTmpSRTFilePath(EncodeFileKey key, int langindex) const {
    return regtmp(StringFormat(_T("%s/c%d-%d-%d-%d%s.srt"),
        tmpDir.path(), key.video, key.format, key.div, langindex, GetCMSuffix(key.cm)));
}

tstring ConfigWrapper::getTmpAMTSourcePath(int vindex) const {
    return regtmp(StringFormat(_T("%s/amts%d.dat"), tmpDir.path(), vindex));
}

tstring ConfigWrapper::getTmpSourceAVS8bitPath(int vindex) const {
    return regtmp(StringFormat(_T("%s/amts%d_8bit.avs"), tmpDir.path(), vindex));
}

tstring ConfigWrapper::getTmpSourceAVSPath(int vindex) const {
    return regtmp(StringFormat(_T("%s/amts%d.avs"), tmpDir.path(), vindex));
}

tstring ConfigWrapper::getTmpLogoFramePath(int vindex, int logoIndex) const {
    if (logoIndex == -1) {
        return regtmp(StringFormat(_T("%s/logof%d.txt"), tmpDir.path(), vindex));
    }
    return regtmp(StringFormat(_T("%s/logof%d-%d.txt"), tmpDir.path(), vindex, logoIndex));
}

tstring ConfigWrapper::getTmpChapterExePath(int vindex) const {
    return regtmp(StringFormat(_T("%s/chapter_exe%d.txt"), tmpDir.path(), vindex));
}

tstring ConfigWrapper::getTmpChapterExeOutPath(int vindex) const {
    return regtmp(StringFormat(_T("%s/chapter_exe_o%d.txt"), tmpDir.path(), vindex));
}

tstring ConfigWrapper::getTmpTrimAVSPath(int vindex) const {
    return regtmp(StringFormat(_T("%s/trim%d.avs"), tmpDir.path(), vindex));
}

tstring ConfigWrapper::getTmpJlsPath(int vindex) const {
    return regtmp(StringFormat(_T("%s/jls%d.txt"), tmpDir.path(), vindex));
}

tstring ConfigWrapper::getTmpDivPath(int vindex) const {
    return regtmp(StringFormat(_T("%s/div%d.txt"), tmpDir.path(), vindex));
}

tstring ConfigWrapper::getTmpChapterPath(EncodeFileKey key) const {
    return regtmp(StringFormat(_T("%s/chapter%d-%d-%d%s.txt"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm)));
}

tstring ConfigWrapper::getTmpNicoJKXMLPath() const {
    return regtmp(StringFormat(_T("%s/nicojk.xml"), tmpDir.path()));
}

tstring ConfigWrapper::getTmpNicoJKASSPath(NicoJKType type) const {
    return regtmp(StringFormat(_T("%s/nicojk%s.ass"), tmpDir.path(), GetNicoJKSuffix(type)));
}

tstring ConfigWrapper::getTmpNicoJKASSPath(EncodeFileKey key, NicoJKType type) const {
    return regtmp(StringFormat(_T("%s/nicojk%d-%d-%d%s%s.ass"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm), GetNicoJKSuffix(type)));
}

tstring ConfigWrapper::getVfrTmpFile1Path(EncodeFileKey key, ENUM_FORMAT format) const {
    return regtmp(StringFormat(_T("%s/t1%d-%d-%d%s.%s"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm), getOutputExtention(format)));
}

tstring ConfigWrapper::getVfrTmpFile2Path(EncodeFileKey key, ENUM_FORMAT format) const {
    return regtmp(StringFormat(_T("%s/t2%d-%d-%d%s.%s"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm), getOutputExtention(format)));
}

tstring ConfigWrapper::getM2tsMetaFilePath(EncodeFileKey key) const {
    return regtmp(StringFormat(_T("%s/t%d-%d-%d%s.meta"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm)));
}

const tchar* ConfigWrapper::getOutputExtention(ENUM_FORMAT format) const {
    switch (format) {
    case FORMAT_MP4: return _T("mp4");
    case FORMAT_MKV: return _T("mkv");
    case FORMAT_M2TS: return _T("m2ts");
    case FORMAT_TS: return _T("ts");
    case FORMAT_TSREPLACE: return _T("ts");
    }
    return _T("amatsukze");
}

tstring ConfigWrapper::getOutFileBaseWithoutPrefix() const {
    return conf.outVideoPath;
}

tstring ConfigWrapper::getOutFileBase(EncodeFileKey key, EncodeFileKey keyMax, ENUM_FORMAT format, VIDEO_STREAM_FORMAT codec) const {
    StringBuilderT sb;
    sb.append(_T("%s"), conf.outVideoPath);
    if (key.format > 0) {
        sb.append(_T("-%d"), key.format);
    }
    if (keyMax.div > 1) {
        sb.append(_T("_div%d"), key.div + 1);
    }
    sb.append(_T("%s"), GetCMSuffix(key.cm));
    if (format == FORMAT_TSREPLACE) {
        switch (codec) {
        case VS_H264: sb.append(_T(".h264")); break;
        case VS_H265: sb.append(_T(".hevc")); break;
        case VS_AV1: sb.append(_T(".av1")); break;
        default:sb.append(_T(".replace")); break;
        }
    }
    return sb.str();
}

tstring ConfigWrapper::getOutFilePath(EncodeFileKey key, EncodeFileKey keyMax, ENUM_FORMAT format, VIDEO_STREAM_FORMAT codec) const {
    return getOutFileBase(key, keyMax, format, codec) + tstring(_T(".")) + getOutputExtention(format);
}

tstring ConfigWrapper::getOutASSPath(EncodeFileKey key, EncodeFileKey keyMax, ENUM_FORMAT format, VIDEO_STREAM_FORMAT codec, int langidx, NicoJKType jktype) const {
    StringBuilderT sb;
    sb.append(_T("%s"), getOutFileBase(key, keyMax, format, codec));
    if (langidx < 0) {
        sb.append(_T("-nicojk%s"), GetNicoJKSuffix(jktype));
    } else if (langidx > 0) {
        sb.append(_T("-%d"), langidx);
    }
    sb.append(_T(".ass"));
    return sb.str();
}

tstring ConfigWrapper::getOutChapterPath(EncodeFileKey key, EncodeFileKey keyMax, ENUM_FORMAT format, VIDEO_STREAM_FORMAT codec) const {
    StringBuilderT sb;
    sb.append(_T("%s"), getOutFileBase(key, keyMax, format, codec));
    sb.append(_T(".chapter.txt"));
    return sb.str();
}

tstring ConfigWrapper::getOutSummaryPath() const {
    return StringFormat(_T("%s.txt"), conf.outVideoPath);
}

tstring ConfigWrapper::getDRCSMapPath() const {
    return conf.drcsMapPath;
}

tstring ConfigWrapper::getDRCSOutPath(const std::string& md5) const {
    return StringFormat(_T("%s/%s.bmp"), conf.drcsOutPath, md5);
}

bool ConfigWrapper::isDumpFilter() const {
    return conf.dumpFilter;
}

tstring ConfigWrapper::getFilterGraphDumpPath(EncodeFileKey key) const {
    return regtmp(StringFormat(_T("%s/graph%d-%d-%d%s.txt"),
        tmpDir.path(), key.video, key.format, key.div, GetCMSuffix(key.cm)));
}

bool ConfigWrapper::isZoneAvailable() const {
    return conf.encoder == ENCODER_X264 || conf.encoder == ENCODER_X265 || conf.encoder == ENCODER_NVENC || conf.encoder == ENCODER_QSVENC;
}

bool ConfigWrapper::isZoneWithoutBitrateAvailable() const {
    return conf.encoder == ENCODER_X264 || conf.encoder == ENCODER_X265;
}

bool ConfigWrapper::isZoneWithQualityAvailable() const {
    return conf.encoder == ENCODER_NVENC || conf.encoder == ENCODER_QSVENC;
}

bool ConfigWrapper::isEncoderSupportVFR() const {
    return conf.encoder == ENCODER_X264;
}

bool ConfigWrapper::isBitrateCMEnabled() const {
    return conf.bitrateCM != 1.0 || conf.cmQualityOffset != 0.0;
}

tstring ConfigWrapper::getOptions(
    int numFrames,
    VIDEO_STREAM_FORMAT srcFormat, double srcBitrate, bool pulldown,
    int pass, const std::vector<BitrateZone>& zones, const tstring& optionFilePath, double vfrBitrateScale,
    EncodeFileKey key, const EncoderOptionInfo& eoInfo) const {
    StringBuilderT sb;
    sb.append(_T("%s"), conf.encoderOptions);
    double targetBitrate = 0;
    if (conf.autoBitrate) {
        targetBitrate = conf.bitrate.getTargetBitrate(srcFormat, srcBitrate);
        if (isEncoderSupportVFR() == false) {
            // �^�C���R�[�h��Ή��G���R�[�_�ɂ�����r�b�g���[�g��VFR����
            targetBitrate *= vfrBitrateScale;
        }
        if (key.cm == CMTYPE_CM && !isZoneAvailable()) {
            targetBitrate *= conf.bitrateCM;
        }
        double maxBitrate = std::max(targetBitrate * 2, srcBitrate);
        if (conf.encoder == ENCODER_QSVENC) {
            sb.append(_T(" --la %d --maxbitrate %d"), (int)targetBitrate, (int)maxBitrate);
        } else if (conf.encoder == ENCODER_NVENC) {
            sb.append(_T(" --vbrhq %d --maxbitrate %d"), (int)targetBitrate, (int)maxBitrate);
        } else if (conf.encoder == ENCODER_VCEENC) {
            sb.append(_T(" --vbr %d --max-bitrate %d"), (int)targetBitrate, (int)maxBitrate);
        } else if (conf.encoder == ENCODER_SVTAV1) {
            sb.append(_T(" -rc 2 -tbr %d"), (int)(targetBitrate * 1000));
        } else {
            sb.append(_T(" --bitrate %d --vbv-maxrate %d --vbv-bufsize %d"),
                (int)targetBitrate, (int)maxBitrate, (int)maxBitrate);
        }
    }
    if (pass >= 0) {
        sb.append(_T(" --pass %d --stats \"%s\""),
            pass, getEncStatsFilePath(key));
    }
    if (zones.size() &&
        isZoneAvailable() && // �G���R�[�_��--zones/--dynamic-rc�ɑΉ����Ă��邩?
        (isEncoderSupportVFR() == false || isBitrateCMEnabled())) { // VFR�������K�v ���邢�� CM�r�b�g���[�g����(�i���I�t�Z�b�g�܂�)���K�v
        if (isZoneWithoutBitrateAvailable()) { // x264/x265
            //ctx.info("getOptions: ApplyZone x264/x265");
            // x264/265
            // �����ł�zone.bitrate�͔{���̈Ӗ��A1.0�Ȃ疳��
            // �L����zone�̎w�肪���邩�T��
            if (std::find_if(zones.begin(), zones.end(), [](const auto& z) { return z.bitrate != 1.0; }) != zones.end()) {
                sb.append(_T(" --zones "));
                bool zoneAdded = false;
                for (int i = 0; i < (int)zones.size(); ++i) {
                    const auto& zone = zones[i];
                    if (zone.bitrate != 1.0) { 
                        sb.append(_T("%s%d,%d,b=%.3g"), (zoneAdded) ? "/" : "",
                            zone.startFrame, zone.endFrame - 1, zone.bitrate);
                        zoneAdded = true;
                    }
                }
            }
        } else if (isZoneWithQualityAvailable() && optionFilePath.length() > 0) {
            //ctx.info("getOptions: ApplyZone QSVEnc/NVEnc");
            // QSVEnc/NVEnc
            if (conf.autoBitrate) {
                // --dynamic-rc���������������ɔ����A�t�@�C���n������
                std::unique_ptr<FILE, decltype(&fclose)> fp(_tfopen(optionFilePath.c_str(), _T("w")), fclose);
                for (int i = 0; i < (int)zones.size(); ++i) {
                    const auto& zone = zones[i];
                    fprintf(fp.get(), " --dynamic-rc %d:%d,vbr=%d\n",
                        zone.startFrame, zone.endFrame - 1, (int)std::round(targetBitrate * zone.bitrate));
                }
                sb.append(_T(" --option-file \"%s\""), optionFilePath);
            } else if (auto rcMode = getRCMode(conf.encoder, eoInfo.rcMode); rcMode) {
                // --dynamic-rc���������������ɔ����A�t�@�C���n������
                bool addOptFileCmd = false;
                std::unique_ptr<FILE, decltype(&fclose)> fp(_tfopen(optionFilePath.c_str(), _T("w")), fclose);
                if (rcMode->isBitrateMode) {
                    for (int i = 0; i < (int)zones.size(); ++i) {
                        const auto& zone = zones[i];
                        fprintf(fp.get(), " --dynamic-rc %d:%d,%s=%d\n",
                            zone.startFrame, zone.endFrame - 1, rcMode->name,
                            (int)std::round(eoInfo.rcModeValue[0] * zone.bitrate));
                        addOptFileCmd = true;
                    }
                } else {
                    for (int i = 0; i < (int)zones.size(); ++i) {
                        const auto& zone = zones[i];
                        if (zone.qualityOffset == 0.0) continue;
                        addOptFileCmd = true;
                        if (std::string(rcMode->name) == "cqp") {
                            fprintf(fp.get(), " --dynamic-rc %d:%d,%s=%d:%d:%d\n",
                                zone.startFrame, zone.endFrame - 1, rcMode->name,
                                std::min(std::max((int)std::round(eoInfo.rcModeValue[0] + zone.qualityOffset), rcMode->valueMin), rcMode->valueMax),
                                std::min(std::max((int)std::round(eoInfo.rcModeValue[1] + zone.qualityOffset), rcMode->valueMin), rcMode->valueMax),
                                std::min(std::max((int)std::round(eoInfo.rcModeValue[2] + zone.qualityOffset), rcMode->valueMin), rcMode->valueMax));
                        } else if (rcMode->isFloat) {
                            fprintf(fp.get(), " --dynamic-rc %d:%d,%s=%f\n",
                                zone.startFrame, zone.endFrame - 1, rcMode->name,
                                std::min(std::max(eoInfo.rcModeValue[0] + zone.qualityOffset, (double)rcMode->valueMin), (double)rcMode->valueMax));
                        } else {
                            fprintf(fp.get(), " --dynamic-rc %d:%d,%s=%d\n",
                                zone.startFrame, zone.endFrame - 1, rcMode->name,
                                std::min(std::max((int)std::round(eoInfo.rcModeValue[0] + zone.qualityOffset), rcMode->valueMin), rcMode->valueMax));
                        }
                    }
                }
                if (addOptFileCmd) {
                    sb.append(_T(" --option-file \"%s\""), optionFilePath);
                }
            }
        }
    }
    // x264/x265��--zones�ŕi���I�t�Z�b�g�͎w��ł��Ȃ��A�܂�SVT-AV1�ɂ͂�������zones���Ȃ�
    // �������ACM��������--crf�𒼐ڏ㏑�����邱�ƂőΉ��\
    if (key.cm == CMTYPE_CM
        && (conf.encoder == ENCODER_X264 || conf.encoder == ENCODER_X265 || conf.encoder == ENCODER_SVTAV1)) {
        //ctx.infoF("getOptions: ApplyZone CM eoInfo.rcMode %s, cmQualityOffset %f", eoInfo.rcMode, conf.cmQualityOffset);
        if (auto rcMode = getRCMode(conf.encoder, eoInfo.rcMode); rcMode && !rcMode->isBitrateMode && conf.cmQualityOffset != 0.0) {
            const tstring rcModeName = to_tstring(rcMode->name);
            if (rcMode->isFloat) {
                sb.append(_T(" --%s %f"), rcModeName,
                    std::min(std::max(eoInfo.rcModeValue[0] + conf.cmQualityOffset, (double)rcMode->valueMin), (double)rcMode->valueMax));
            } else {
                sb.append(_T(" --%s %d"), rcModeName,
                    std::min(std::max((int)std::round(eoInfo.rcModeValue[0] + conf.cmQualityOffset), rcMode->valueMin), rcMode->valueMax));
            }
        }
    }
    if (numFrames > 0) {
        switch (conf.encoder) {
        case ENCODER_X264:
        case ENCODER_X265:
        case ENCODER_QSVENC:
        case ENCODER_NVENC:
        case ENCODER_VCEENC:
            sb.append(_T(" --frames %d"), numFrames);
            break;
        case ENCODER_SVTAV1:
            sb.append(_T(" -n %d"), numFrames);
            break;
        default:
            break;
        }
    }
    return sb.str();
}

void ConfigWrapper::dump() const {
    ctx.info("[�ݒ�]");
    if (conf.mode != _T("ts")) {
        ctx.infoF("Mode: %s", conf.mode);
    }
    ctx.infoF("����: %s", conf.srcFilePath);
    if (conf.srcFilePath != conf.srcFilePathOrg) {
        ctx.infoF("���� (�I���W�i��): %s", conf.srcFilePathOrg);
    }
    ctx.infoF("�o��: %s", conf.outVideoPath);
    ctx.infoF("�ꎞ�t�H���_: %s", tmpDir.path());
    ctx.infoF("�o�̓t�H�[�}�b�g: %s%s",
        formatToString(conf.format),
        (conf.useMKVWhenSubExist) ? " (��������ł�MKV)" : "");
    ctx.infoF("�G���R�[�_: %s (%s)", conf.encoderPath, encoderToString(conf.encoder));
    ctx.infoF("�G���R�[�_�I�v�V����: %s", conf.encoderOptions);
    if (conf.userSAR.first > 0 && conf.userSAR.second > 0) {
        ctx.infoF("���[�U�[�w��SAR: %d:%d", conf.userSAR.first, conf.userSAR.second);
    }
    if (conf.autoBitrate) {
        ctx.infoF("�����r�b�g���[�g: �L�� (%g:%g:%g)",
            conf.bitrate.a, conf.bitrate.b, conf.bitrate.h264);
    } else {
        ctx.info("�����r�b�g���[�g: ����");
    }
    ctx.infoF("�G���R�[�h/�o��: %s/%s",
        conf.twoPass ? "2�p�X" : "1�p�X",
        cmOutMaskToString(conf.cmoutmask));
    ctx.infoF("�`���v�^�[���: %s%s",
        conf.chapter ? "�L��" : "����",
        (conf.chapter && conf.ignoreNoLogo) ? "" : "�i���S�K�{�j");
    if (conf.chapter) {
        for (int i = 0; i < (int)conf.logoPath.size(); ++i) {
            ctx.infoF("logo%d: %s", (i + 1), conf.logoPath[i]);
        }
    }
    ctx.infoF("���S����: %s", conf.noDelogo ? "���Ȃ�" : "����");
    ctx.infoF("���񃍃S���: %s", conf.parallelLogoAnalysis ? "�I��" : "�I�t");
    if (conf.audioEncoder != AUDIO_ENCODER_NONE) {
        ctx.infoF("����: %s (%s)", conf.audioEncoderPath, audioEncoderToString(conf.audioEncoder));
        if (conf.audioBitrateInKbps > 0) {
            ctx.infoF("�����G���R�[�_�r�b�g���[�g: %d kbps", conf.audioBitrateInKbps);
        }
        ctx.infoF("�����G���R�[�_�I�v�V����: %s", conf.audioEncoderOptions);
    }
    ctx.infoF("����: %s", conf.subtitles ? "�L��" : "����");
    if (conf.subtitles) {
        ctx.infoF("DRCS�}�b�s���O: %s", conf.drcsMapPath);
    }
    if (conf.serviceId > 0) {
        ctx.infoF("�T�[�r�XID: %d", conf.serviceId);
    } else {
        ctx.info("�T�[�r�XID: �w��Ȃ�");
    }
    ctx.infoF("�f�R�[�_: MPEG2:%s H264:%s HEVC:%s",
        decoderToString(conf.decoderSetting.mpeg2),
        decoderToString(conf.decoderSetting.h264),
        decoderToString(conf.decoderSetting.hevc));
}

void ConfigWrapper::CreateTempDir() {
    tmpDir.Initialize();
}

const char* ConfigWrapper::decoderToString(DECODER_TYPE decoder) const {
    switch (decoder) {
    case DECODER_QSV: return "QSV";
    case DECODER_CUVID: return "CUVID";
    }
    return "default";
}

const char* ConfigWrapper::formatToString(ENUM_FORMAT fmt) const {
    switch (fmt) {
    case FORMAT_MP4: return "MP4";
    case FORMAT_MKV: return "Matroska";
    case FORMAT_M2TS: return "M2TS";
    case FORMAT_TS: return "TS";
    case FORMAT_TSREPLACE: return "TS (replace)";
    }
    return "unknown";
}

tstring ConfigWrapper::regtmp(tstring str) const {
    ctx.registerTmpFile(str);
    return str;
}
