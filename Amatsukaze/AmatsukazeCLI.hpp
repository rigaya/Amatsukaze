/**
* Amtasukaze Command Line Interface
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <time.h>

#include "TranscodeManager.h"
#include "AmatsukazeTestImpl.h"
#include "Version.h"

// MSVC�̃}���`�o�C�g��Unicode�łȂ��̂ŕ����񑀍�ɓK���Ȃ��̂�wchar_t�ŕ����񑀍������


static void printCopyright() {
    PRINTF(
        "Amatsukaze - Automated MPEG2-TS Transcoder %s (%s %s)\n"
        "Copyright (c) 2017-2019 Nekopanda\n", AMATSUKAZE_VERSION, __DATE__, __TIME__);
}

static void printHelp(const tchar* bin) {
    PRINTF(
        "%" PRITSTR " <�I�v�V����> -i <input.ts> -o <output.mp4>\n"
        "�I�v�V���� []�̓f�t�H���g�l \n"
        "  -i|--input  <�p�X>  ���̓t�@�C���p�X\n"
        "  -o|--output <�p�X>  �o�̓t�@�C���p�X\n"
        "  -s|--serviceid <���l> ��������T�[�r�XID���w��[]\n"
        "  -w|--work   <�p�X>  �ꎞ�t�@�C���p�X[./]\n"
        "  -et|--encoder-type <�^�C�v>  �g�p�G���R�[�_�^�C�v[x264]\n"
        "                      �Ή��G���R�[�_: x264,x265,QSVEnc,NVEnc,VCEEnc,SVT-AV1\n"
        "  -e|--encoder <�p�X> �G���R�[�_�p�X[x264.exe]\n"
        "  -eo|--encoder-option <�I�v�V����> �G���R�[�_�֓n���I�v�V����[]\n"
        "                      ���̓t�@�C���̉𑜓x�A�A�X�y�N�g��A�C���^���[�X�t���O�A\n"
        "                      �t���[�����[�g�A�J���[�}�g���N�X���͎����Œǉ������̂ŕs�v\n"
        "  --sar w:h           SAR��̏㏑�� (SVT-AV1�g�p���̂ݗL��)\n"
        "  -b|--bitrate a:b:f  �r�b�g���[�g�v�Z�� �f���r�b�g���[�gkbps = f*(a*s+b)\n"
        "                      s�͓��͉f���r�b�g���[�g�Af�͓��͂�H264�̏ꍇ�͓��͂��ꂽf�����A\n"
        "                      ���͂�MPEG2�̏ꍇ��f=1�Ƃ���\n"
        "                      �w�肪�Ȃ��ꍇ�̓r�b�g���[�g�I�v�V������ǉ����Ȃ�\n"
        "  -bcm|--bitrate-cm <float>   CM���肳�ꂽ�Ƃ���̃r�b�g���[�g�{��\n"
        "  --cm-quality-offset <float> CM���肳�ꂽ�Ƃ���̕i���I�t�Z�b�g\n"
        "  --2pass             2pass�G���R�[�h\n"
        "  --splitsub          ���C���ȊO�̃t�H�[�}�b�g�͌������Ȃ�\n"
        "  -aet|--audio-encoder-type <�^�C�v> �����G���R�[�_[]"
        "                      �Ή��G���R�[�_: neroAac, qaac, fdkaac, opusenc\n"
        "                      �w�肵�Ȃ���Ή����̓G���R�[�h���Ȃ�\n"
        "  -ae|--audio-encoder <�p�X> �����G���R�[�_[]"
        "  -aeo|--audio-encoder-option <�I�v�V����> �����G���R�[�_�֓n���I�v�V����[]\n"
        "  -fmt|--format <�t�H�[�}�b�g> �o�̓t�H�[�}�b�g[mp4]\n"
        "                      �Ή��t�H�[�}�b�g: mp4,mkv,m2ts,ts\n"
        "  --use-mkv-when-sub-exists ����������ꍇ�ɂ�mkv�o�͂���������\n"
        "  -m|--muxer  <�p�X>  L-SMASH��muxer�܂���mkvmerge�܂���tsMuxeR�ւ̃p�X[muxer.exe]\n"
        "  -t|--timelineeditor  <�p�X>  timelineeditor�ւ̃p�X�iMP4��VFR�o�͂���ꍇ�ɕK�v�j[timelineeditor.exe]\n"
        "  --mp4box <�p�X>     mp4box�ւ̃p�X�iMP4�Ŏ�����������ꍇ�ɕK�v�j[mp4box.exe]\n"
        "  --mkvmerge <�p�X>   mkvmerge�ւ̃p�X�i--use-mkv-when-sub-exists�g�p���ɕK�v�j[mkvmerge.exe]\n"
        "  --tsreplace-remove-typed  tsreplace���s����--remove-typed���w�肷��\n"
        "  -f|--filter <�p�X>  �t�B���^Avisynth�X�N���v�g�ւ̃p�X[]\n"
        "  -pf|--postfilter <�p�X>  �|�X�g�t�B���^Avisynth�X�N���v�g�ւ̃p�X[]\n"
        "  --mpeg2decoder <�f�R�[�_>  MPEG2�p�f�R�[�_[default]\n"
        "                      �g�p�\�f�R�[�_: default,QSV,CUVID\n"
        "  --h264decoder <�f�R�[�_>  H264�p�f�R�[�_[default]\n"
        "                      �g�p�\�f�R�[�_: default,QSV,CUVID\n"
        "  --chapter           �`���v�^�[�ECM��͂��s��\n"
        "  --subtitles         ��������������\n"
        "  --nicojk            �j�R�j�R�����R�����g��ǉ�����\n"
        "  --logo <�p�X>       ���S�t�@�C�����w��i�����ł��w��\�j\n"
        "  --erase-logo <�p�X> ���S�����p�ǉ����S�t�@�C���B���S�����ɓK�p����܂��B�i�����ł��w��\�j\n"
        "  --drcs <�p�X>       DRCS�}�b�s���O�t�@�C���p�X\n"
        "  --ignore-no-drcsmap �}�b�s���O�ɂȂ�DRCS�O���������Ă������𑱍s����\n"
        "  --ignore-no-logo    ���S��������Ȃ��Ă������𑱍s����\n"
        "  --ignore-nicojk-error �j�R�j�R�����擾�ŃG���[���������Ă������𑱍s����\n"
        "  --no-delogo         ���S���������Ȃ��i�f�t�H���g�̓��S������ꍇ�͏����܂��j\n"
        "  --parallel-logo-analysis ���񃍃S���\n"
        "  --loose-logo-detection ���S���o���肵�����l��Ⴍ���܂�\n"
        "  --max-fade-length <���l> ���S�̍ő�t�F�[�h�t���[����[16]\n"
        "  --chapter-exe <�p�X> chapter_exe.exe�ւ̃p�X\n"
        "  --jls <�p�X>         join_logo_scp.exe�ւ̃p�X\n"
        "  --jls-cmd <�p�X>    join_logo_scp�̃R�}���h�t�@�C���ւ̃p�X\n"
        "  --jls-option <�I�v�V����>    join_logo_scp�̃R�}���h�t�@�C���ւ̃p�X\n"
        "  --trimavs <�p�X>    CM�J�b�g�pTrim AVS�t�@�C���ւ̃p�X�B���C���t�@�C����CM�J�b�g�o�͂ł̂ݎg�p�����B\n"
        "  --nicoass <�p�X>     NicoConvASS�ւ̃p�X\n"
        "  -om|--cmoutmask <���l> �o�̓}�X�N[1]\n"
        "                      1 : �ʏ�\n"
        "                      2 : CM���J�b�g\n"
        "                      4 : CM�̂ݏo��\n"
        "                      OR���� ��) 6: �{�҂�CM�𕪗�\n"
        "  --nicojk18          �j�R�j�R�����R�����g��nicojk18�T�[�o����擾\n"
        "  --nicojklog         �j�R�j�R�����R�����g��NicoJK���O�t�H���_����擾\n"
        "                      (NicoConvASS�� -nicojk 1 �ŌĂяo���܂�)\n"
        "  --nicojkmask <���l> �j�R�j�R�����R�����g�}�X�N[1]\n"
        "                      1 : 1280x720�s����\n"
        "                      2 : 1280x720������\n"
        "                      4 : 1920x1080�s����\n"
        "                      8 : 1920x1080������\n"
        "                      OR���� ��) 15: ���ׂďo��\n"
        "  --no-remove-tmp     �ꎞ�t�@�C�����폜�����Ɏc��\n"
        "                      �f�t�H���g��60fps�^�C�~���O�Ő���\n"
        "  --timefactor <���l>  x265��NVEnc�ŋ^��VFR���[�g�R���g���[������Ƃ��̎��ԃ��[�g�t�@�N�^�[[0.25]\n"
        "  --pmt-cut <���l>:<���l>  PMT�ύX��CM�F������Ƃ��̍ő�CM�F�����Ԋ����B�S�Đ����Ԃɑ΂��銄���Ŏw�肷��B\n"
        "                      �Ⴆ�� 0.1:0.2 �Ƃ���ƊJ�n10%%�܂ł�PMT�ύX���������ꍇ�͂���PMT�ύX�܂ł�CM�F������B\n"
        "                      �܂��I��肩��20%%�܂ł�PMT�ύX���������ꍇ�����l��CM�F������B[0:0]\n"
        "  -j|--json   <�p�X>  �o�͌��ʏ���JSON�o�͂���ꍇ�͏o�̓t�@�C���p�X���w��[]\n"
        "  --mode <���[�h>     �������[�h[ts]\n"
        "                      ts : MPGE2-TS����͂���ʏ�G���R�[�h���[�h\n"
        "                      cm : �G���R�[�h�܂ōs�킸�ACM��͂܂łŏI�����郂�[�h\n"
        "                      drcs : �}�b�s���O�̂Ȃ�DRCS�O���摜�����o�͂��郂�[�h\n"
        "                      probe_subtitles : ���������邩����\n"
        "                      probe_audio : �����t�H�[�}�b�g���o��\n"
        "  --resource-manager <���̓p�C�v>:<�o�̓p�C�v> ���\�[�X�Ǘ��z�X�g�Ƃ̒ʐM�p�C�v\n"
        "  --affinity <�O���[�v>:<�}�X�N> CPU�A�t�B�j�e�B\n"
        "                      �O���[�v�̓v���Z�b�T�O���[�v�i64�_���R�A�ȉ��̃V�X�e���ł�0�̂݁j\n"
        "  --max-frames        probe_*���[�h���̂ݗL���BTS�����鎞�Ԃ��f���t���[�����Ŏw��[9000]\n"
        "  --dump              �����r���̃f�[�^���_���v�i�f�o�b�O�p�j\n",
        bin);
}

static tstring getParam(int argc, const tchar* argv[], int ikey) {
    if (ikey + 1 >= argc) {
        THROWF(FormatException,
            "%" PRITSTR "�I�v�V�����̓p�����[�^���K�v�ł�", argv[ikey]);
    }
    return argv[ikey + 1];
}

static ENUM_ENCODER encoderFtomString(const tstring& str) {
    if (str == _T("x264")) {
        return ENCODER_X264;
    } else if (str == _T("x265")) {
        return ENCODER_X265;
    } else if (str == _T("qsv") || str == _T("QSVEnc")) {
        return ENCODER_QSVENC;
    } else if (str == _T("nvenc") || str == _T("NVEnc")) {
        return ENCODER_NVENC;
    } else if (str == _T("vceenc") || str == _T("VCEEnc")) {
        return ENCODER_VCEENC;
    } else if (str == _T("svt-av1") || str == _T("SVT-AV1")) {
        return ENCODER_SVTAV1;
    }
    return (ENUM_ENCODER)-1;
}

static ENUM_AUDIO_ENCODER audioEncoderFtomString(const tstring& str) {
    if (str == _T("neroAac")) {
        return AUDIO_ENCODER_NEROAAC;
    } else if (str == _T("qaac")) {
        return AUDIO_ENCODER_QAAC;
    } else if (str == _T("fdkaac")) {
        return AUDIO_ENCODER_FDKAAC;
    } else if (str == _T("opusenc")) {
        return AUDIO_ENCODER_OPUSENC;
    }
    return (ENUM_AUDIO_ENCODER)-1;
}

static DECODER_TYPE decoderFromString(const tstring& str) {
    if (str == _T("default")) {
        return DECODER_DEFAULT;
    } else if (str == _T("qsv") || str == _T("QSV")) {
        return DECODER_QSV;
    } else if (str == _T("cuvid") || str == _T("CUVID") || str == _T("nvdec") || str == _T("NVDec")) {
        return DECODER_CUVID;
    }
    return (DECODER_TYPE)-1;
}

static std::unique_ptr<ConfigWrapper> parseArgs(AMTContext& ctx, int argc, const tchar* argv[]) {
    tstring moduleDir = pathNormalize(GetModuleDirectory());
    Config conf = Config();
    conf.workDir = _T("./");
    conf.encoderPath = _T("x264.exe");
    conf.encoderOptions = _T("");
    conf.timelineditorPath = _T("timelineeditor.exe");
    conf.mp4boxPath = _T("mp4box.exe");
    conf.mkvmergePath = _T("mkvmerge.exe");
    conf.chapterExePath = _T("chapter_exe.exe");
    conf.joinLogoScpPath = _T("join_logo_scp.exe");
    conf.nicoConvAssPath = _T("NicoConvASS.exe");
    conf.nicoConvChSidPath = _T("ch_sid.txt");
    conf.drcsOutPath = moduleDir + _T("/../drcs");
    conf.drcsMapPath = conf.drcsOutPath + _T("/drcs_map.txt");
    conf.joinLogoScpCmdPath = moduleDir + _T("/../JL/JL_�W��.txt");
    conf.mode = _T("ts");
    conf.modeArgs = _T("");
    conf.userSAR = { 0, 0 };
    conf.bitrateCM = 1.0;
    conf.cmQualityOffset = 0.0;
    conf.x265TimeFactor = 0.25;
    conf.serviceId = -1;
    conf.cmoutmask = 1;
    conf.nicojkmask = 1;
    conf.maxframes = 30 * 300;
    conf.inPipe = INVALID_HANDLE_VALUE;
    conf.outPipe = INVALID_HANDLE_VALUE;
    conf.maxFadeLength = 16;
    conf.numEncodeBufferFrames = 16;
    conf.tsreplaceRemoveTypeD = false;
    conf.useMKVWhenSubExist = false;
    bool nicojk = false;

    for (int i = 1; i < argc; ++i) {
        tstring key = argv[i];
        if (key == _T("--loadv2")) {
            ; // skip
        } else if (key == _T("-i") || key == _T("--input")) {
            conf.srcFilePath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("--original-input-file")) {
            conf.srcFilePathOrg = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("-o") || key == _T("--output")) {
            conf.outVideoPath =
                pathRemoveExtension(pathNormalize(getParam(argc, argv, i++)));
        } else if (key == _T("--mode")) {
            conf.mode = getParam(argc, argv, i++);
        } else if (key == _T("-a") || key == _T("--args")) {
            conf.modeArgs = getParam(argc, argv, i++);
        } else if (key == _T("-w") || key == _T("--work")) {
            conf.workDir = pathNormalize(getParam(argc, argv, i++));
            if (conf.workDir.size() == 0) {
                conf.workDir = _T("./");
            }
        } else if (key == _T("-et") || key == _T("--encoder-type")) {
            tstring arg = getParam(argc, argv, i++);
            conf.encoder = encoderFtomString(arg);
            if (conf.encoder == (ENUM_ENCODER)-1) {
                PRINTF("--encoder-type�̎w�肪�Ԉ���Ă��܂�: %" PRITSTR "\n", arg.c_str());
            }
        } else if (key == _T("-e") || key == _T("--encoder")) {
            conf.encoderPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("-eo") || key == _T("--encoder-option")) {
            conf.encoderOptions = getParam(argc, argv, i++);
        } else if (key == _T("--sar")) {
            const auto arg = getParam(argc, argv, i++);
            int ret = sscanfT(arg.c_str(), _T("%d:%d"), &conf.userSAR.first, &conf.userSAR.second);
            if (ret < 2) {
                THROWF(ArgumentException, "--sar�̎w�肪�Ԉ���Ă��܂�");
            }
        } else if (key == _T("-aet") || key == _T("--audio-encoder-type")) {
            tstring arg = getParam(argc, argv, i++);
            conf.audioEncoder = audioEncoderFtomString(arg);
            if (conf.audioEncoder == (ENUM_AUDIO_ENCODER)-1) {
                PRINTF("--audio-encoder-type�̎w�肪�Ԉ���Ă��܂�: %" PRITSTR "\n", arg.c_str());
            }
        } else if (key == _T("-ae") || key == _T("--audio-encoder")) {
            conf.audioEncoderPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("-aeo") || key == _T("--audio-encoder-option")) {
            conf.audioEncoderOptions = getParam(argc, argv, i++);
        } else if (key == _T("-ab") || key == _T("--audio-bitrate")) {
            conf.audioBitrateInKbps = std::stoi(getParam(argc, argv, i++));
        } else if (key == _T("-b") || key == _T("--bitrate")) {
            const auto arg = getParam(argc, argv, i++);
            int ret = sscanfT(arg.c_str(), _T("%lf:%lf:%lf:%lf"),
                &conf.bitrate.a, &conf.bitrate.b, &conf.bitrate.h264, &conf.bitrate.h265);
            if (ret < 3) {
                THROWF(ArgumentException, "--bitrate�̎w�肪�Ԉ���Ă��܂�");
            }
            if (ret <= 3) {
                conf.bitrate.h265 = 2;
            }
            conf.autoBitrate = true;
        } else if (key == _T("-bcm") || key == _T("--bitrate-cm")) {
            const auto arg = getParam(argc, argv, i++);
            int ret = sscanfT(arg.c_str(), _T("%lf"), &conf.bitrateCM);
            if (ret == 0) {
                THROWF(ArgumentException, "--bitrate-cm�̎w�肪�Ԉ���Ă��܂�");
            }
        } else if (key == _T("--cm-quality-offset")) {
            const auto arg = getParam(argc, argv, i++);
            int ret = sscanfT(arg.c_str(), _T("%lf"), &conf.cmQualityOffset);
            if (ret == 0) {
                THROWF(ArgumentException, "--cm-quality-offset�̎w�肪�Ԉ���Ă��܂�");
            }
        } else if (key == _T("--2pass")) {
            conf.twoPass = true;
        } else if (key == _T("--splitsub")) {
            conf.splitSub = true;
        } else if (key == _T("-fmt") || key == _T("--format")) {
            const auto arg = getParam(argc, argv, i++);
            if (arg == _T("mp4")) {
                conf.format = FORMAT_MP4;
            } else if (arg == _T("mkv")) {
                conf.format = FORMAT_MKV;
            } else if (arg == _T("m2ts")) {
                conf.format = FORMAT_M2TS;
            } else if (arg == _T("ts")) {
                conf.format = FORMAT_TS;
            } else if (arg == _T("tsreplace")) {
                conf.format = FORMAT_TSREPLACE;
            } else {
                THROWF(ArgumentException, "--format�̎w�肪�Ԉ���Ă��܂�: %" PRITSTR "", arg);
            }
        } else if (key == _T("--tsreplace-remove-typed")) {
            conf.tsreplaceRemoveTypeD = true;
        } else if (key == _T("--use-mkv-when-sub-exists")) {
            conf.useMKVWhenSubExist = true;
        } else if (key == _T("--chapter")) {
            conf.chapter = true;
        } else if (key == _T("--subtitles")) {
            conf.subtitles = true;
        } else if (key == _T("--nicojk")) {
            nicojk = true;
        } else if (key == _T("-m") || key == _T("--muxer")) {
            conf.muxerPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("-t") || key == _T("--timelineeditor")) {
            conf.timelineditorPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("--mp4box")) {
            conf.mp4boxPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("--mkvmerge")) {
            conf.mkvmergePath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("-j") || key == _T("--json")) {
            conf.outInfoJsonPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("-f") || key == _T("--filter")) {
            conf.filterScriptPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("-pf") || key == _T("--postfilter")) {
            conf.postFilterScriptPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("-s") || key == _T("--serivceid")) {
            tstring sidstr = getParam(argc, argv, i++);
            if (sidstr.size() > 2 && sidstr.substr(0, 2) == _T("0x")) {
                // 16�i
                conf.serviceId = std::stoi(sidstr.substr(2), NULL, 16);;
            } else {
                // 10�i
                conf.serviceId = std::stoi(sidstr);
            }
        } else if (key == _T("--mpeg2decoder")) {
            tstring arg = getParam(argc, argv, i++);
            conf.decoderSetting.mpeg2 = decoderFromString(arg);
            if (conf.decoderSetting.mpeg2 == (DECODER_TYPE)-1) {
                PRINTF("--mpeg2decoder�̎w�肪�Ԉ���Ă��܂�: %" PRITSTR "\n", arg.c_str());
            }
        } else if (key == _T("--h264decoder")) {
            tstring arg = getParam(argc, argv, i++);
            conf.decoderSetting.h264 = decoderFromString(arg);
            if (conf.decoderSetting.h264 == (DECODER_TYPE)-1) {
                PRINTF("--h264decoder�̎w�肪�Ԉ���Ă��܂�: %" PRITSTR "\n", arg.c_str());
            }
        } else if (key == _T("--hevcdecoder")) {
            tstring arg = getParam(argc, argv, i++);
            conf.decoderSetting.hevc = decoderFromString(arg);
            if (conf.decoderSetting.hevc == (DECODER_TYPE)-1) {
                PRINTF("--hevcdecoder�̎w�肪�Ԉ���Ă��܂�: %" PRITSTR "\n", arg.c_str());
            }
        } else if (key == _T("-eb") || key == _T("--encode-buffer")) {
            conf.numEncodeBufferFrames = std::stoi(getParam(argc, argv, i++));
        } else if (key == _T("--ignore-no-logo")) {
            conf.ignoreNoLogo = true;
        } else if (key == _T("--ignore-no-drcsmap")) {
            conf.ignoreNoDrcsMap = true;
        } else if (key == _T("--ignore-nicojk-error")) {
            conf.ignoreNicoJKError = true;
        } else if (key == _T("--loose-logo-detection")) {
            conf.looseLogoDetection = true;
        } else if (key == _T("--max-fade-length")) {
            conf.maxFadeLength = std::stoi(getParam(argc, argv, i++));
        } else if (key == _T("--no-delogo")) {
            conf.noDelogo = true;
        } else if (key == _T("--parallel-logo-analysis")) {
            conf.parallelLogoAnalysis = true;
        } else if (key == _T("--timefactor")) {
            const auto arg = getParam(argc, argv, i++);
            int ret = sscanfT(arg.c_str(), _T("%lf"), &conf.x265TimeFactor);
            if (ret == 0) {
                THROWF(ArgumentException, "--timefactor�̎w�肪�Ԉ���Ă��܂�");
            }
        } else if (key == _T("--logo")) {
            conf.logoPath.push_back(pathNormalize(getParam(argc, argv, i++)));
        } else if (key == _T("--erase-logo")) {
            conf.eraseLogoPath.push_back(pathNormalize(getParam(argc, argv, i++)));
        } else if (key == _T("--drcs")) {
            auto path = pathNormalize(getParam(argc, argv, i++));
            conf.drcsMapPath = path;
            conf.drcsOutPath = pathGetDirectory(path);
            if (conf.drcsOutPath.size() == 0) {
                conf.drcsOutPath = _T(".");
            }
        } else if (key == _T("--chapter-exe")) {
            conf.chapterExePath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("--chapter-exe-options")) {
            conf.chapterExeOptions = getParam(argc, argv, i++);
        } else if (key == _T("--jls")) {
            conf.joinLogoScpPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("--jls-cmd")) {
            conf.joinLogoScpCmdPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("--jls-option")) {
            conf.joinLogoScpOptions = getParam(argc, argv, i++);
        } else if (key == _T("--trimavs")) {
            conf.trimavsPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("--nicoass")) {
            conf.nicoConvAssPath = pathNormalize(getParam(argc, argv, i++));
        } else if (key == _T("--nicojk18")) {
            conf.nicojk18 = true;
        } else if (key == _T("--nicojklog")) {
            conf.useNicoJKLog = true;
        } else if (key == _T("-om") || key == _T("--cmoutmask")) {
            conf.cmoutmask = std::stol(getParam(argc, argv, i++));
        } else if (key == _T("--nicojkmask")) {
            conf.nicojkmask = std::stol(getParam(argc, argv, i++));
        } else if (key == _T("--dump")) {
            conf.dumpStreamInfo = true;
        } else if (key == _T("--systemavsplugin")) {
            conf.systemAvsPlugin = true;
        } else if (key == _T("--no-remove-tmp")) {
            conf.noRemoveTmp = true;
        } else if (key == _T("--dump-filter")) {
            conf.dumpFilter = true;
        } else if (key == _T("--resource-manager")) {
            const auto arg = getParam(argc, argv, i++);
            size_t inPipe, outPipe;
            int ret = sscanfT(arg.c_str(), _T("%zu:%zu"), &inPipe, &outPipe);
            if (ret < 2) {
                THROWF(ArgumentException, "--resource-manager�̎w�肪�Ԉ���Ă��܂�");
            }
            conf.inPipe = (HANDLE)inPipe;
            conf.outPipe = (HANDLE)outPipe;
        } else if (key == _T("--affinity")) {
            const auto arg = getParam(argc, argv, i++);
            int ret = sscanfT(arg.c_str(), _T("%d:%lld"), &conf.affinityGroup, &conf.affinityMask);
            if (ret < 2) {
                THROWF(ArgumentException, "--affinity�̎w�肪�Ԉ���Ă��܂�");
            }
        } else if (key == _T("--max-frames")) {
            conf.maxframes = std::stoi(getParam(argc, argv, i++));
        } else if (key == _T("--pmt-cut")) {
            const auto arg = getParam(argc, argv, i++);
            int ret = sscanfT(arg.c_str(), _T("%lf:%lf"),
                &conf.pmtCutSideRate[0], &conf.pmtCutSideRate[1]);
            if (ret < 2) {
                THROWF(ArgumentException, "--pmt-cut�̎w�肪�Ԉ���Ă��܂�");
            }
        } else if (key == _T("--print-prefix")) {
            const auto arg = getParam(argc, argv, i++);
            if (arg == _T("default")) {
                conf.printPrefix = AMT_PREFIX_DEFAULT;
            } else if (arg == _T("time")) {
                conf.printPrefix = AMT_PREFIX_TIME;
            } else {
                THROWF(ArgumentException, "--print-prefix�̎w�肪�Ԉ���Ă��܂�");
            }
        } else if (key.size() == 0) {
            continue;
        } else {
            // �Ȃ���%ls�Œ���������H�킷�Ɨ�����̂�%s�ŕ\��
            THROWF(FormatException, "�s���ȃI�v�V����: %s", to_string(argv[i]));
        }
    }

    if (!nicojk) {
        conf.nicojkmask = 0;
    }

    // muxer�̃f�t�H���g�l
    if (conf.muxerPath.size() == 0) {
        if (conf.format == FORMAT_MP4) {
            conf.muxerPath = _T("muxer.exe");
        } else if (conf.format == FORMAT_MKV) {
            conf.muxerPath = _T("mkvmerge.exe");
        } else if (conf.format == FORMAT_TSREPLACE) {
            conf.muxerPath = _T("tsreplace.exe");
        } else {
            conf.muxerPath = _T("tsmuxer.exe");
        }
    }

    if (conf.mode == _T("ts") || conf.mode == _T("g")) {
        if (conf.srcFilePath.size() == 0) {
            THROWF(ArgumentException, "���̓t�@�C�����w�肵�Ă�������");
        }
        if (conf.outVideoPath.size() == 0) {
            THROWF(ArgumentException, "�o�̓t�@�C�����w�肵�Ă�������");
        }
    }

    if (conf.mode == _T("drcs") || conf.mode == _T("cm") || starts_with(conf.mode, _T("probe_"))) {
        if (conf.srcFilePath.size() == 0) {
            THROWF(ArgumentException, "���̓t�@�C�����w�肵�Ă�������");
        }
    }

    if (conf.chapter && !conf.ignoreNoLogo) {
        if (conf.logoPath.size() == 0) {
            THROW(ArgumentException, "���S���w�肳��Ă��܂���");
        }
    }

    // CM��͂͂S������K�v������
    if (conf.chapterExePath.size() > 0 || conf.joinLogoScpPath.size() > 0) {
        if (conf.chapterExePath.size() == 0) {
            THROW(ArgumentException, "chapter_exe.exe�ւ̃p�X���ݒ肳��Ă��܂���");
        }
        if (conf.joinLogoScpPath.size() == 0) {
            THROW(ArgumentException, "join_logo_scp.exe�ւ̃p�X���ݒ肳��Ă��܂���");
        }
    }

    if (conf.maxFadeLength < 0) {
        THROW(ArgumentException, "max-fade-length���s��");
    }

    if (conf.mode == _T("enctask")) {
        // �K�v�Ȃ�
        conf.workDir = _T("");
    }

    // exe��T��
    if (conf.mode != _T("drcs") && !starts_with(conf.mode, _T("probe_"))) {
        auto search = [](const tstring& path) {
            return pathNormalize(SearchExe(path));
            };
        conf.chapterExePath = search(conf.chapterExePath);
        conf.encoderPath = search(conf.encoderPath);
        conf.joinLogoScpPath = search(conf.joinLogoScpPath);
        conf.nicoConvAssPath = search(conf.nicoConvAssPath);
        conf.nicoConvChSidPath = pathGetDirectory(conf.nicoConvAssPath) + _T("/ch_sid.txt");
        conf.mp4boxPath = search(conf.mp4boxPath);
        conf.mkvmergePath = search(conf.mkvmergePath);
        conf.muxerPath = search(conf.muxerPath);
        conf.timelineditorPath = search(conf.timelineditorPath);
    }

    if (conf.srcFilePathOrg.size() == 0) {
        conf.srcFilePathOrg = conf.srcFilePath;
    }

    return std::unique_ptr<ConfigWrapper>(new ConfigWrapper(ctx, conf));
}

static CRITICAL_SECTION g_log_crisec;
static void amatsukaze_av_log_callback(
    void* ptr, int level, const char* fmt, va_list vl) {
    level &= 0xff;

    if (level > av_log_get_level()) {
        return;
    }

    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    int len = (int)strlen(buf);
    if (len == 0) {
        return;
    }

    static char* log_levels[] = {
        "panic", "fatal", "error", "warn", "info", "verb", "debug", "trace"
    };

    EnterCriticalSection(&g_log_crisec);

    static bool print_prefix = true;
    bool tmp_pp = print_prefix;
    print_prefix = (buf[len - 1] == '\r' || buf[len - 1] == '\n');
    if (tmp_pp) {
        int logtype = level / 8;
        const char* level_str =
            (logtype >= sizeof(log_levels) / sizeof(log_levels[0]))
            ? "unk" : log_levels[logtype];
        fprintf(stderr, "FFMPEG [%s] %s", level_str, buf);
    } else {
        fprintf(stderr, buf);
    }
    if (print_prefix) {
        fflush(stderr);
    }

    LeaveCriticalSection(&g_log_crisec);
}

static int amatsukazeTranscodeMain(AMTContext& ctx, const ConfigWrapper& setting) {
    try {

        if (setting.isSubtitlesEnabled()) {
            // DRCS�}�b�s���O�����[�h
            ctx.loadDRCSMapping(setting.getDRCSMapPath());
        }

        tstring mode = setting.getMode();
        if (mode == _T("ts") || mode == _T("cm"))
            transcodeMain(ctx, setting);
        else if (mode == _T("g"))
            transcodeSimpleMain(ctx, setting);
        else if (mode == _T("drcs"))
            searchDrcsMain(ctx, setting);
        else if (mode == _T("probe_subtitles"))
            detectSubtitleMain(ctx, setting);
        else if (mode == _T("probe_audio"))
            detectAudioMain(ctx, setting);

        else if (mode == _T("test_print_crc"))
            test::PrintCRCTable(ctx, setting);
        else if (mode == _T("test_crc"))
            test::CheckCRC(ctx, setting);
        else if (mode == _T("test_read_bits"))
            test::ReadBits(ctx, setting);
        else if (mode == _T("test_auto_buffer"))
            test::CheckAutoBuffer(ctx, setting);
        else if (mode == _T("test_verifympeg2ps"))
            test::VerifyMpeg2Ps(ctx, setting);
        else if (mode == _T("test_readts"))
            test::ReadTS(ctx, setting);
        else if (mode == _T("test_aacdec"))
            test::AacDecode(ctx, setting);
        else if (mode == _T("test_wavewrite"))
            test::WaveWriteHeader(ctx, setting);
        else if (mode == _T("test_process"))
            test::ProcessTest(ctx, setting);
        else if (mode == _T("test_streamreform"))
            test::FileStreamInfo(ctx, setting);
        else if (mode == _T("test_parseargs"))
            test::ParseArgs(ctx, setting);
        else if (mode == _T("test_logoframe"))
            test::LogoFrameTest(ctx, setting);
        else if (mode == _T("test_dualmono"))
            test::SplitDualMonoAAC(ctx, setting);
        else if (mode == _T("test_aacdecode"))
            test::AACDecodeTest(ctx, setting);
        else if (mode == _T("test_ass"))
            test::CaptionASS(ctx, setting);
        else if (mode == _T("test_eo"))
            test::EncoderOptionParse(ctx, setting);
        else if (mode == _T("test_perf"))
            test::DecodePerformance(ctx, setting);
        else if (mode == _T("test_zone"))
            test::BitrateZones(ctx, setting);
        else if (mode == _T("test_zone2"))
            test::BitrateZonesBug(ctx, setting);
        else if (mode == _T("test_printf"))
            test::PrintfBug(ctx, setting);
        else if (mode == _T("test_resource"))
            test::ResourceTest(ctx, setting);

        else
            ctx.errorF("--mode�̎w�肪�Ԉ���Ă��܂�: %s\n", mode.c_str());

        return 0;
    } catch (const NoLogoException&) {
        // ���S������100�Ƃ���
        return 100;
    } catch (const NoDrcsMapException&) {
        // DRCS�}�b�s���O�Ȃ���101�Ƃ���
        return 101;
    } catch (const AvisynthError& avserror) {
        ctx.error("AviSynth Error");
        ctx.error(avserror.msg);
        return 2;
    } catch (const Exception&) {
        return 1;
    }
}

int RunAmatsukazeCLI(int argc, const wchar_t* argv[]) {
    try {
        printCopyright();

        AMTContext ctx;

        ctx.setDefaultCP();

        auto setting = parseArgs(ctx, argc, argv);

        ctx.setTimePrefix(setting->getPrintPrefix() == AMT_PREFIX_TIME);

        // CPU�A�t�B�j�e�B��ݒ�
        if (!SetCPUAffinity(setting->getAffinityGroup(), setting->getAffinityMask())) {
            ctx.error("CPU�A�t�B�j�e�B��ݒ�ł��܂���ł���");
        }

        // FFMPEG���C�u����������
        InitializeCriticalSection(&g_log_crisec);
        av_log_set_callback(amatsukaze_av_log_callback);
        //av_register_all();

        // �L���v�V����DLL������
        InitializeCPW();

        return amatsukazeTranscodeMain(ctx, *setting);
    } catch (const Exception&) {
        // parseArgs�ŃG���[
        printHelp(argv[0]);
        return 1;
    }
}
