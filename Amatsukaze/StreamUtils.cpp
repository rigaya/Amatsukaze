/**
* Memory and Stream utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/

#include "StreamUtils.h"

#include "rgy_util.h"

const char* CMTypeToString(CMType cmtype) {
    if (cmtype == CMTYPE_CM) return "CM";
    if (cmtype == CMTYPE_NONCM) return "本編";
    return "";
}

const char* PictureTypeString(PICTURE_TYPE pic) {
    switch (pic) {
    case PIC_FRAME: return "FRAME";
    case PIC_FRAME_DOUBLING: return "DBL";
    case PIC_FRAME_TRIPLING: return "TLP";
    case PIC_TFF: return "TFF";
    case PIC_BFF: return "BFF";
    case PIC_TFF_RFF: return "TFF_RFF";
    case PIC_BFF_RFF: return "BFF_RFF";
    default: return "UNK";
    }
}

const char* FrameTypeString(FRAME_TYPE frame) {
    switch (frame) {
    case FRAME_I: return "I";
    case FRAME_P: return "P";
    case FRAME_B: return "B";
    default: return "UNK";
    }
}

double presenting_time(PICTURE_TYPE picType, double frameRate) {
    switch (picType) {
    case PIC_FRAME: return 1.0 / frameRate;
    case PIC_FRAME_DOUBLING: return 2.0 / frameRate;
    case PIC_FRAME_TRIPLING: return 3.0 / frameRate;
    case PIC_TFF: return 1.0 / frameRate;
    case PIC_BFF: return 1.0 / frameRate;
    case PIC_TFF_RFF: return 1.5 / frameRate;
    case PIC_BFF_RFF: return 1.5 / frameRate;
    default: break;
    }
    // 不明
    return 1.0 / frameRate;
}

const char* getAudioChannelString(AUDIO_CHANNELS channels) {
    switch (channels) {
    case AUDIO_MONO: return "モノラル";
    case AUDIO_STEREO: return "ステレオ";
    case AUDIO_30: return "3/0";
    case AUDIO_31: return "3/1";
    case AUDIO_32: return "3/2";
    case AUDIO_32_LFE: return "5.1ch";
    case AUDIO_21: return "2/1";
    case AUDIO_22: return "2/2";
    case AUDIO_2LANG: return "デュアルモノ";
    case AUDIO_52_LFE: return "7.1ch";
    case AUDIO_33_LFE: return "3/3.1";
    case AUDIO_2_22_LFE: return "2/0/0-2/0/2-0.1";
    case AUDIO_322_LFE: return "3/2/2.1";
    case AUDIO_2_32_LFE: return "2/0/0-3/0/2-0.1";
    case AUDIO_020_32_LFE: return "0/2/0-3/0/2-0.1";
    case AUDIO_2_323_2LFE: return "2/0/0-3/2/3-0.2";
    case AUDIO_333_523_3_2LFE: return "22.2ch";
    default: break;
    }
    return "エラー";
}

int getNumAudioChannels(AUDIO_CHANNELS channels) {
    switch (channels) {
    case AUDIO_MONO: return 1;
    case AUDIO_STEREO: return 2;
    case AUDIO_30: return 3;
    case AUDIO_31: return 4;
    case AUDIO_32: return 5;
    case AUDIO_32_LFE: return 6;
    case AUDIO_21: return 3;
    case AUDIO_22: return 4;
    case AUDIO_2LANG: return 2;
    case AUDIO_52_LFE: return 8;
    case AUDIO_33_LFE: return 7;
    case AUDIO_2_22_LFE: return 7;
    case AUDIO_322_LFE: return 8;
    case AUDIO_2_32_LFE: return 8;
    case AUDIO_020_32_LFE: return 8;
    case AUDIO_2_323_2LFE: return 12;
    case AUDIO_333_523_3_2LFE: return 24;
    default: break;
    }
    return 2; // 不明
}

void DeleteScriptEnvironment(IScriptEnvironment2* env) {
    if (env) env->DeleteScriptEnvironment();
}

ScriptEnvironmentPointer make_unique_ptr(IScriptEnvironment2* env) {
    return ScriptEnvironmentPointer(env, DeleteScriptEnvironment);
}

void ConcatFiles(const std::vector<tstring>& srcpaths, const tstring& dstpath) {
    enum { BUF_SIZE = 16 * 1024 * 1024 };
    auto buf = std::unique_ptr<uint8_t[]>(new uint8_t[BUF_SIZE]);
    File dstfile(dstpath, _T("wb"));
    for (int i = 0; i < (int)srcpaths.size(); ++i) {
        File srcfile(srcpaths[i], _T("rb"));
        while (true) {
            size_t readBytes = srcfile.read(MemoryChunk(buf.get(), BUF_SIZE));
            dstfile.write(MemoryChunk(buf.get(), readBytes));
            if (readBytes != BUF_SIZE) break;
        }
    }
}

// BOMありUTF8で搗き撼む
void WriteUTF8File(const tstring& filename, const std::string& utf8text) {
    File file(filename, _T("w"));
    uint8_t bom[] = { 0xEF, 0xBB, 0xBF };
    file.write(MemoryChunk(bom, sizeof(bom)));
    file.write(MemoryChunk((uint8_t*)utf8text.data(), utf8text.size()));
}

void WriteUTF8File(const tstring& filename, const std::wstring& text) {
    WriteUTF8File(filename, wstring_to_string(text, CP_UTF8));
}

// C API for P/Invoke
extern "C" AMATSUKAZE_API AMTContext * AMTContext_Create() { return new AMTContext(); }
extern "C" AMATSUKAZE_API void ATMContext_Delete(AMTContext * ptr) { delete ptr; }
extern "C" AMATSUKAZE_API const char* AMTContext_GetError(AMTContext * ptr) { return ptr->getError().c_str(); }
