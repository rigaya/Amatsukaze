﻿/**
* Memory and Stream utility
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <algorithm>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <fstream>
#include "rgy_osdep.h"
#include "CoreUtils.hpp"
#include "FileUtils.h"
#include "OSUtil.h"
#include "StringUtils.h"
#include "rgy_util.h"

enum {
    TS_SYNC_BYTE = 0x47,

    TS_PACKET_LENGTH = 188,
    TS_PACKET_LENGTH2 = 192,

    MAX_PID = 0x1FFF,

    MPEG_CLOCK_HZ = 90000, // MPEG2,H264,H265はPTSが90kHz単位となっている
};

inline static int nblocks(int n, int block) {
    return (n + block - 1) / block;
}

/** @brief shiftだけ右シフトしてmask数bitだけ返す(bit shift mask) */
template <typename T>
T bsm(T v, int shift, int mask) {
    return (v >> shift) & ((T(1) << mask) - 1);
}

/** @brief mask数bitだけshiftだけ左シフトして書き込む(bit mask shift) */
template <typename T, typename U>
void bms(T& v, U data, int shift, int mask) {
    v |= (data & ((T(1) << mask) - 1)) << shift;
}

template<int bytes, typename T>
T readN(const uint8_t* ptr) {
    T r = 0;
    for (int i = 0; i < bytes; ++i) {
        r = r | (T(ptr[i]) << ((bytes - i - 1) * 8));
    }
    return r;
}
static uint16_t read16(const uint8_t* ptr) { return readN<2, uint16_t>(ptr); }
static uint32_t read24(const uint8_t* ptr) { return readN<3, uint32_t>(ptr); }
static uint32_t read32(const uint8_t* ptr) { return readN<4, uint32_t>(ptr); }
static uint64_t read40(const uint8_t* ptr) { return readN<5, uint64_t>(ptr); }
static uint64_t read48(const uint8_t* ptr) { return readN<6, uint64_t>(ptr); }

template<int bytes, typename T>
void writeN(uint8_t* ptr, T w) {
    for (int i = 0; i < bytes; ++i) {
        ptr[i] = uint8_t(w >> ((bytes - i - 1) * 8));
    }
}
static void write16(uint8_t* ptr, uint16_t w) { writeN<2, uint16_t>(ptr, w); }
static void write24(uint8_t* ptr, uint32_t w) { writeN<3, uint32_t>(ptr, w); }
static void write32(uint8_t* ptr, uint32_t w) { writeN<4, uint32_t>(ptr, w); }
static void write40(uint8_t* ptr, uint64_t w) { writeN<5, uint64_t>(ptr, w); }
static void write48(uint8_t* ptr, uint64_t w) { writeN<6, uint64_t>(ptr, w); }


class BitReader {
public:
    BitReader(MemoryChunk data)
        : data(data)
        , offset(0)
        , filled(0) {
        fill();
    }

    bool canRead(int bits) {
        return (filled + ((int)data.length - offset) * 8) >= bits;
    }

    // bitsビット読んで進める
    // bits <= 32
    template <int bits>
    uint32_t read() {
        return readn(bits);
    }

    uint32_t readn(int bits) {
        if (bits <= filled) {
            return read_(bits);
        }
        fill();
        if (bits > filled) {
            throw EOFException("BitReader.readでオーバーラン");
        }
        return read_(bits);
    }

    // bitsビット読むだけ
    // bits <= 32
    template <int bits>
    uint32_t next() {
        return nextn(bits);
    }

    uint32_t nextn(int bits) {
        if (bits <= filled) {
            return next_(bits);
        }
        fill();
        if (bits > filled) {
            throw EOFException("BitReader.nextでオーバーラン");
        }
        return next_(bits);
    }

    int32_t readExpGolomSigned() {
        static int table[] = { 1, -1 };
        uint32_t v = readExpGolom() + 1;
        return (v >> 1) * table[v & 1];
    }

    uint32_t readExpGolom() {
        uint64_t masked = bsm(current, 0, filled);
        if (masked == 0) {
            fill();
            masked = bsm(current, 0, filled);
            if (masked == 0) {
                throw EOFException("BitReader.readExpGolomでオーバーラン");
            }
        }
        int bodyLen = filled - __builtin_clzl(masked);
        filled -= bodyLen - 1;
        if (bodyLen > filled) {
            fill();
            if (bodyLen > filled) {
                throw EOFException("BitReader.readExpGolomでオーバーラン");
            }
        }
        int shift = filled - bodyLen;
        filled -= bodyLen;
        return (uint32_t)bsm(current, shift, bodyLen) - 1;
    }

    void skip(int bits) {
        if (filled > bits) {
            filled -= bits;
        } else {
            // 今fillされている分を引く
            bits -= filled;
            filled = 0;
            // これでバイトアラインされるので残りバイト数スキップ
            int skipBytes = bits / 8;
            offset += skipBytes;
            if (offset > (int)data.length) {
                throw EOFException("BitReader.skipでオーバーラン");
            }
            bits -= skipBytes * 8;
            // もう1回fillして残ったビット数を引く
            fill();
            if (bits > filled) {
                throw EOFException("BitReader.skipでオーバーラン");
            }
            filled -= bits;
        }
    }

    // 次のバイト境界までのビットを捨てる
    void byteAlign() {
        fill();
        filled &= ~7;
    }

    // 読んだバイト数（中途半端な部分まで読んだ場合も１バイトとして計算）
    int numReadBytes() {
        return offset - (filled / 8);
    }

private:
    MemoryChunk data;
    int offset;
    uint64_t current;
    int filled;

    void fill() {
        while (filled + 8 <= 64 && offset < (int)data.length) readByte();
    }

    void readByte() {
        current = (current << 8) | data.data[offset++];
        filled += 8;
    }

    uint32_t read_(int bits) {
        int shift = filled - bits;
        filled -= bits;
        return (uint32_t)bsm(current, shift, bits);
    }

    uint32_t next_(int bits) {
        int shift = filled - bits;
        return (uint32_t)bsm(current, shift, bits);
    }
};

class BitWriter {
public:
    BitWriter(AutoBuffer& dst)
        : dst(dst)
        , current(0)
        , filled(0) {}

    void writen(uint32_t data, int bits) {
        if (filled + bits > 64) {
            store();
        }
        current |= (((uint64_t)data & ((uint64_t(1) << bits) - 1)) << (64 - filled - bits));
        filled += bits;
    }

    template <int bits>
    void write(uint32_t data) {
        writen(data, bits);
    }

    template <bool bits>
    void byteAlign() {
        int pad = ((filled + 7) & ~7) - filled;
        filled += pad;
        if (bits) {
            current |= ((uint64_t(1) << pad) - 1) << (64 - filled);
        }
    }

    void flush() {
        if (filled & 7) {
            THROW(FormatException, "バイトアラインしていません");
        }
        store();
    }

private:
    AutoBuffer& dst;
    uint64_t current;
    int filled;

    void storeByte() {
        dst.add(uint8_t(current >> 56));
        current <<= 8;
        filled -= 8;
    }

    void store() {
        while (filled >= 8) storeByte();
    }
};

class CRC32 {
public:
    CRC32() {
        createTable(table, 0x04C11DB7UL);
    }

    uint32_t calc(const uint8_t* data, int length, uint32_t crc) const {
        for (int i = 0; i < length; ++i) {
            crc = (crc << 8) ^ table[(crc >> 24) ^ data[i]];
        }
        return crc;
    }

    const uint32_t* getTable() const { return table; }

private:
    uint32_t table[256];

    static void createTable(uint32_t* table, uint32_t exp) {
        for (int i = 0; i < 256; ++i) {
            uint32_t crc = i << 24;
            for (int j = 0; j < 8; ++j) {
                if (crc & 0x80000000UL) {
                    crc = (crc << 1) ^ exp;
                } else {
                    crc = crc << 1;
                }
            }
            table[i] = crc;
        }
    }
};

enum AMT_LOG_LEVEL {
    AMT_LOG_DEBUG,
    AMT_LOG_INFO,
    AMT_LOG_WARN,
    AMT_LOG_ERROR
};

enum AMT_ERROR_COUNTER {
    // 不明なPTSのフレーム
    AMT_ERR_UNKNOWN_PTS = 0,
    // PESパケットデコードエラー
    AMT_ERR_DECODE_PACKET_FAILED,
    // H264におけるPTSミスマッチ
    AMT_ERR_H264_PTS_MISMATCH,
    // H264におけるフィールド配置エラー
    AMT_ERR_H264_UNEXPECTED_FIELD,
    // PTSが戻っている
    AMT_ERR_NON_CONTINUOUS_PTS,
    // DRCSマッピングがない
    AMT_ERR_NO_DRCS_MAP,
    // 音声でコードエラー
    AMT_ERR_DECODE_AUDIO,
    // エラーの個数
    AMT_ERR_MAX,
};

static const char* AMT_ERROR_NAMES[] = {
   "unknown-pts",
   "decode-packet-failed",
   "h264-pts-mismatch",
   "h264-unexpected-field",
   "non-continuous-pts",
     "no-drcs-map",
     "decode-audio-failed",
};

class AMTContext {
public:
    AMTContext()
        : timePrefix(true)
#if defined(_WIN32) || defined(_WIN64)
        , acp(GetACP())
#else
        , acp(CP_UTF8)
#endif
        , errCounter() {}

    const CRC32* getCRC() const {
        return &crc;
    }

    // ！！入力文字列を含む文字列をfmtに渡すのは禁止！！
    // （%が含まれていると誤動作するので）

    void debug(const char *str) const {
        print(str, AMT_LOG_DEBUG);
    }
    template <typename ... Args>
    void debugF(const char *fmt, const Args& ... args) const {
        print(StringFormat(fmt, args ...).c_str(), AMT_LOG_DEBUG);
    }
    void info(const char *str) const {
        print(str, AMT_LOG_INFO);
    }
    template <typename ... Args>
    void infoF(const char *fmt, const Args& ... args) const {
        print(StringFormat(fmt, args ...).c_str(), AMT_LOG_INFO);
    }
    void warn(const char *str) const {
        print(str, AMT_LOG_WARN);
    }
    template <typename ... Args>
    void warnF(const char *fmt, const Args& ... args) const {
        print(StringFormat(fmt, args ...).c_str(), AMT_LOG_WARN);
    }
    void error(const char *str) const {
        print(str, AMT_LOG_ERROR);
    }
    template <typename ... Args>
    void errorF(const char *fmt, const Args& ... args) const {
        print(StringFormat(fmt, args ...).c_str(), AMT_LOG_ERROR);
    }
    void progress(const char *str) const {
        printProgress(str);
    }
    template <typename ... Args>
    void progressF(const char *fmt, const Args& ... args) const {
        printProgress(StringFormat(fmt, args ...).c_str());
    }

    void registerTmpFile(const tstring& path) {
        tmpFiles.insert(path);
    }

    void clearTmpFiles() {
        for (auto& path : tmpFiles) {
            if (path.find(_T('*')) != tstring::npos) {
                auto dir = pathGetDirectory(path);
                for (auto name : GetDirectoryFiles(dir, path.substr(dir.size() + 1))) {
                    auto path2 = dir + _T("/") + name;
                    removeT(path2.c_str());
                }
            } else {
                removeT(path.c_str());
            }
        }
        tmpFiles.clear();
    }

    void incrementCounter(AMT_ERROR_COUNTER err) {
        errCounter[err]++;
    }

    int getErrorCount(AMT_ERROR_COUNTER err) const {
        return errCounter[err];
    }

    void setError(const Exception& exception) {
        errMessage = exception.message();
    }

    const std::string& getError() const {
        return errMessage;
    }

    void setTimePrefix(bool enable) {
        timePrefix = enable;
    }

    const std::map<std::string, std::wstring>& getDRCSMapping() const {
        return drcsMap;
    }

    void loadDRCSMapping(const tstring& mapPath) {
        if (File::exists(mapPath) == false) {
            THROWF(ArgumentException, "DRCSマッピングファイルが見つかりません: %s",
                mapPath.c_str());
        } else {
            // BOMありUTF-8で読み込む
            std::ifstream input(mapPath);
            // BOMスキップ
            {
                const char a = input.get();
                const char b = input.get();
                const char c = input.get();
                if (a != (char)0xEF || b != (char)0xBB || c != (char)0xBF) { //UTF-8 BOM
                    input.seekg(0);
                }
            }
            for (std::string line; getline(input, line);) {
                if (line.size() >= 34) {
                    std::string key = line.substr(0, 32);
                    std::transform(key.begin(), key.end(), key.begin(), toupper);
                    bool ok = (line[32] == '=');
                    for (auto c : key) if (!isxdigit(c)) ok = false;
                    if (ok) {
                        drcsMap[key] = char_to_wstring(line.substr(33), CP_UTF8);
                    }
                }
            }
        }
    }

    // コンソール出力をデフォルトコードページに設定
    void setDefaultCP() {
#if defined(_WIN32) || defined(_WIN64)
        SetConsoleCP(acp);
        SetConsoleOutputCP(acp);
#endif
    }

private:
    bool timePrefix;
    CRC32 crc;
    int acp;

    std::set<tstring> tmpFiles;
    std::array<int, AMT_ERR_MAX> errCounter;
    std::string errMessage;

    std::map<std::string, std::wstring> drcsMap;

    void printWithTimePrefix(const char* str) const {
        time_t rawtime;
        char buffer[80];

        time(&rawtime);
        tm * timeinfo = localtime(&rawtime);

        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
        PRINTF("%s %s\n", buffer, str);
    }

    void print(const char* str, AMT_LOG_LEVEL level) const {
        if (timePrefix) {
            printWithTimePrefix(str);
        } else {
            static const char* log_levels[] = { "debug", "info", "warn", "error" };
            PRINTF("AMT [%s] %s\n", log_levels[level], str);
        }
    }

    void printProgress(const char* str) const {
        if (timePrefix) {
            printWithTimePrefix(str);
        } else {
            PRINTF("AMT %s\r", str);
        }
    }
};

class AMTObject {
public:
    AMTObject(AMTContext& ctx) : ctx(ctx) {}
    virtual ~AMTObject() {}
    AMTContext& ctx;
};

enum DECODER_TYPE {
    DECODER_DEFAULT = 0,
    DECODER_QSV,
    DECODER_CUVID,
};

struct DecoderSetting {
    DECODER_TYPE mpeg2;
    DECODER_TYPE h264;
    DECODER_TYPE hevc;

    DecoderSetting()
        : mpeg2(DECODER_DEFAULT)
        , h264(DECODER_DEFAULT)
        , hevc(DECODER_DEFAULT) {}
};

enum CMType {
    CMTYPE_BOTH = 0,
    CMTYPE_NONCM = 1,
    CMTYPE_CM = 2,
    CMTYPE_MAX
};

// 出力ファイル識別
struct EncodeFileKey {
    int video;   // 中間ファイル番号（映像フォーマット切り替えによる分割）
    int format;  // フォーマット番号（音声その他のフォーマット変更による分割）
    int div;     // 分割番号（CM構成認識による分割）
    CMType cm;   // CMタイプ（本編、CMなど）

    explicit EncodeFileKey()
        : video(0), format(0), div(0), cm(CMTYPE_BOTH) {}
    EncodeFileKey(int video, int format)
        : video(video), format(format), div(0), cm(CMTYPE_BOTH) {}
    EncodeFileKey(int video, int format, int div, CMType cm)
        : video(video), format(format), div(div), cm(cm) {}

    int key() const {
        return (video << 24) | (format << 14) | (div << 4) | cm;
    }
};

const char* CMTypeToString(CMType cmtype);

enum VIDEO_STREAM_FORMAT {
    VS_UNKNOWN,
    VS_MPEG2,
    VS_H264,
    VS_H265,
    VS_AV1
};

enum PICTURE_TYPE {
    PIC_FRAME = 0, // progressive frame
    PIC_FRAME_DOUBLING, // frame doubling
    PIC_FRAME_TRIPLING, // frame tripling
    PIC_TFF, // top field first
    PIC_BFF, // bottom field first
    PIC_TFF_RFF, // tff かつ repeat first field
    PIC_BFF_RFF, // bff かつ repeat first field
    MAX_PIC_TYPE,
};

const char* PictureTypeString(PICTURE_TYPE pic);

enum FRAME_TYPE {
    FRAME_NO_INFO = 0,
    FRAME_I,
    FRAME_P,
    FRAME_B,
    FRAME_OTHER,
    MAX_FRAME_TYPE,
};

const char* FrameTypeString(FRAME_TYPE frame);

double presenting_time(PICTURE_TYPE picType, double frameRate);

struct VideoFormat {
    VIDEO_STREAM_FORMAT format;
    int width, height; // フレームの横縦
    int displayWidth, displayHeight; // フレームの内表示領域の縦横（表示領域中心オフセットはゼロと仮定）
    int sarWidth, sarHeight; // アスペクト比
    int frameRateNum, frameRateDenom; // フレームレート
    uint8_t colorPrimaries, transferCharacteristics, colorSpace; // カラースペース
    bool progressive, fixedFrameRate;

    bool isEmpty() const {
        return width == 0;
    }

    bool isSARUnspecified() const {
        return sarWidth == 0 && sarHeight == 1;
    }

    void mulDivFps(int mul, int div) {
        frameRateNum *= mul;
        frameRateDenom *= div;
        int g = gcd(frameRateNum, frameRateDenom);
        frameRateNum /= g;
        frameRateDenom /= g;
    }

    void getDAR(int& darWidth, int& darHeight) const {
        darWidth = displayWidth * sarWidth;
        darHeight = displayHeight * sarHeight;
        int denom = gcd(darWidth, darHeight);
        darWidth /= denom;
        darHeight /= denom;
    }

    // アスペクト比は見ない
    bool isBasicEquals(const VideoFormat& o) const {
        return (width == o.width && height == o.height
            && frameRateNum == o.frameRateNum && frameRateDenom == o.frameRateDenom
            && progressive == o.progressive);
    }

    // アスペクト比も見る
    bool operator==(const VideoFormat& o) const {
        return (isBasicEquals(o)
            && displayWidth == o.displayWidth && displayHeight == o.displayHeight
            && sarWidth == o.sarWidth && sarHeight == o.sarHeight);
    }
    bool operator!=(const VideoFormat& o) const {
        return !(*this == o);
    }

private:
    static int gcd(int u, int v) {
        int r;
        while (0 != v) {
            r = u % v;
            u = v;
            v = r;
        }
        return u;
    }
};

struct VideoFrameInfo {
    int64_t PTS, DTS;
    // MPEG2の場合 sequence header がある
    // H264の場合 SPS がある
    bool isGopStart;
    bool progressive;
    PICTURE_TYPE pic;
    FRAME_TYPE type; // 使わないけど参考情報
    int codedDataSize; // 映像ビットストリームにおけるバイト数
    VideoFormat format;
};

enum AUDIO_CHANNELS {
    AUDIO_NONE,

    AUDIO_MONO,
    AUDIO_STEREO,
    AUDIO_30, // 3/0
    AUDIO_31, // 3/1
    AUDIO_32, // 3/2
    AUDIO_32_LFE, // 5.1ch

    AUDIO_21, // 2/1
    AUDIO_22, // 2/2
    AUDIO_2LANG, // 2 音声 (1/ 0 + 1 / 0)

    // 以下4K向け
    AUDIO_52_LFE, // 7.1ch
    AUDIO_33_LFE, // 3/3.1
    AUDIO_2_22_LFE, // 2/0/0-2/0/2-0.1
    AUDIO_322_LFE, // 3/2/2.1
    AUDIO_2_32_LFE, // 2/0/0-3/0/2-0.1
    AUDIO_020_32_LFE, // 0/2/0-3/0/2-0.1 // AUDIO_2_32_LFEと区別できなくね？
    AUDIO_2_323_2LFE, // 2/0/0-3/2/3-0.2
    AUDIO_333_523_3_2LFE, // 22.2ch
};

const char* getAudioChannelString(AUDIO_CHANNELS channels);

int getNumAudioChannels(AUDIO_CHANNELS channels);

struct AudioFormat {
    AUDIO_CHANNELS channels;
    int sampleRate;

    bool operator==(const AudioFormat& o) const {
        return (channels == o.channels && sampleRate == o.sampleRate);
    }
    bool operator!=(const AudioFormat& o) const {
        return !(*this == o);
    }
};

struct AudioFrameInfo {
    int64_t PTS;
    int numSamples; // 1チャンネルあたりのサンプル数
    AudioFormat format;
};

struct AudioFrameData : public AudioFrameInfo {
    int codedDataSize;
    uint8_t* codedData;
    int numDecodedSamples;
    int decodedDataSize;
    uint16_t* decodedData;
};

class IVideoParser {
public:
    // とりあえず必要な物だけ
    virtual void reset() = 0;

    // PTS, DTS: 90kHzタイムスタンプ 情報がない場合は-1
    virtual bool inputFrame(MemoryChunk frame, std::vector<VideoFrameInfo>& info, int64_t PTS, int64_t DTS) = 0;
};

enum NicoJKType {
    NICOJK_720S = 0,
    NICOJK_720T = 1,
    NICOJK_1080S = 2,
    NICOJK_1080T = 3,
    NICOJK_MAX
};

#include "AviSynthWrapper.h"

void DeleteScriptEnvironment(IScriptEnvironment2* env);

typedef std::unique_ptr<IScriptEnvironment2, decltype(&DeleteScriptEnvironment)> ScriptEnvironmentPointer;

ScriptEnvironmentPointer make_unique_ptr(IScriptEnvironment2* env);

template<typename T>
void CopyYV12(T* dst, const T* srcY, const T* srcU, const T* srcV, int pitchY, int pitchUV, int width, int height) {
    int widthUV = width >> 1;
    int heightUV = height >> 1;

    T* dstp = dst;
    for (int y = 0; y < height; ++y) {
        memcpy(dstp, &srcY[y * pitchY], width * sizeof(T));
        dstp += width;
    }
    for (int y = 0; y < heightUV; ++y) {
        memcpy(dstp, &srcU[y * pitchUV], widthUV * sizeof(T));
        dstp += widthUV;
    }
    for (int y = 0; y < heightUV; ++y) {
        memcpy(dstp, &srcV[y * pitchUV], widthUV * sizeof(T));
        dstp += widthUV;
    }
}

template<typename T>
void CopyYV12(T* dst, PVideoFrame& frame, int width, int height) {
    const T* srcY = (const T* )frame->GetReadPtr(PLANAR_Y);
    const T* srcU = (const T* )frame->GetReadPtr(PLANAR_U);
    const T* srcV = (const T* )frame->GetReadPtr(PLANAR_V);
    int pitchY = frame->GetPitch(PLANAR_Y) / sizeof(T);
    int pitchUV = frame->GetPitch(PLANAR_U) / sizeof(T);
    CopyYV12(dst, srcY, srcU, srcV, pitchY, pitchUV, width, height);
}

//template<typename T>
//void CopyYV12(PVideoFrame& dst, uint8_t* frame, int width, int height) {
//    const T* dstY = (const T* )dst->GetWritePtr(PLANAR_Y);
//    const T* dstU = (const T* )dst->GetWritePtr(PLANAR_U);
//    const T* dstV = (const T* )dst->GetWritePtr(PLANAR_V);
//    int pitchY = dst->GetPitch(PLANAR_Y) / sizeof(T);
//    int pitchUV = dst->GetPitch(PLANAR_U) / sizeof(T);
//    CopyYV12(dst, srcY, srcU, srcV, pitchY, pitchUV, width, height);
//}

void ConcatFiles(const std::vector<tstring>& srcpaths, const tstring& dstpath);

// BOMありUTF8で書き込む
void WriteUTF8File(const tstring& filename, const std::string& utf8text);

void WriteUTF8File(const tstring& filename, const std::wstring& text);
