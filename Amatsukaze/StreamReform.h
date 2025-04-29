#pragma once

/**
* Output stream construction
* Copyright (c) 2017-2019 Nekopanda
*
* This software is released under the MIT License.
* http://opensource.org/licenses/mit-license.php
*/
#pragma once

#include <time.h>

#include <vector>
#include <map>
#include <memory>
#include <functional>

#include "CaptionData.h"
#include "StreamUtils.h"
#include "Mpeg2TsParser.h"

// 時間は全て 90kHz double で計算する
// 90kHzでも60*1000/1001fpsの1フレームの時間は整数で表せない
// だからと言って27MHzでは数値が大きすぎる

struct FileAudioFrameInfo : public AudioFrameInfo {
    int audioIdx;
    int codedDataSize;
    int waveDataSize;
    int64_t fileOffset;
    int64_t waveOffset;

    FileAudioFrameInfo();

    FileAudioFrameInfo(const AudioFrameInfo& info);
};

struct FileVideoFrameInfo : public VideoFrameInfo {
    int64_t fileOffset;

    FileVideoFrameInfo();

    FileVideoFrameInfo(const VideoFrameInfo& info);
};

enum StreamEventType {
    STREAM_EVENT_NONE = 0,
    PID_TABLE_CHANGED,
    VIDEO_FORMAT_CHANGED,
    AUDIO_FORMAT_CHANGED
};

struct StreamEvent {
    StreamEventType type;
    int frameIdx;	// フレーム番号
    int audioIdx;	// 変更された音声インデックス（AUDIO_FORMAT_CHANGEDのときのみ有効）
    int numAudio;	// 音声の数（PID_TABLE_CHANGEDのときのみ有効）
};

typedef std::vector<std::vector<int>> FileAudioFrameList;

struct OutVideoFormat {
    int formatId; // 内部フォーマットID（通し番号）
    int videoFileId;
    VideoFormat videoFormat;
    std::vector<AudioFormat> audioFormat;
};

// 音ズレ統計情報
struct AudioDiffInfo {
    double sumPtsDiff;
    int totalSrcFrames;
    int totalAudioFrames; // 出力した音声フレーム（水増し分を含む）
    int totalUniquAudioFrames; // 出力した音声フレーム（水増し分を含まず）
    double maxPtsDiff;
    double maxPtsDiffPos;
    double basePts;

    // 秒単位で取得
    double avgDiff() const;
    // 秒単位で取得
    double maxDiff() const;

    void printAudioPtsDiff(AMTContext& ctx) const;

    void printToJson(StringBuilder& sb);

private:
    double elapsedTime(double modPTS) const;
};

struct FilterSourceFrame {
    bool halfDelay;
    int frameIndex; // 内部用(DTS順フレーム番号)
    double pts; // 内部用
    double frameDuration; // 内部用
    int64_t framePTS;
    int64_t fileOffset;
    int keyFrame;
    CMType cmType;
};

struct FilterAudioFrame {
    int frameIndex; // デバッグ用
    int64_t waveOffset;
    int waveLength;
};

struct FilterOutVideoInfo {
    int numFrames;
    int frameRateNum;
    int frameRateDenom;
    int fakeAudioSampleRate;
    std::vector<int> fakeAudioSamples;
};

struct OutCaptionLine {
    double start, end;
    CaptionLine* line;
};

typedef std::vector<std::vector<OutCaptionLine>> OutCaptionList;

struct NicoJKLine {
    double start, end;
    std::string line;

    void Write(const File& file) const;

    static NicoJKLine Read(const File& file);
};

typedef std::array<std::vector<NicoJKLine>, NICOJK_MAX> NicoJKList;

typedef std::pair<int64_t, JSTTime> TimeInfo;

struct EncodeFileInput {
    EncodeFileKey key;     // キー
    EncodeFileKey outKey; // 出力ファイル名用キー
    EncodeFileKey keyMax;  // 出力ファイル名決定用最大値
    double duration;       // 再生時間
    std::vector<int> videoFrames; // 映像フレームリスト（中身はフィルタ入力フレームでのインデックス）
    FileAudioFrameList audioFrames; // 音声フレームリスト
    OutCaptionList captionList;     // 字幕
    NicoJKList nicojkList;          // ニコニコ実況コメント
};

class StreamReformInfo : public AMTObject {
public:
    StreamReformInfo(
        AMTContext& ctx,
        int numVideoFile,
        std::vector<FileVideoFrameInfo>& videoFrameList,
        std::vector<FileAudioFrameInfo>& audioFrameList,
        std::vector<CaptionItem>& captionList,
        std::vector<StreamEvent>& streamEventList,
        std::vector<TimeInfo>& timeList);

    // 1. コンストラクト直後に呼ぶ
    // splitSub: メイン以外のフォーマットを結合しない
    void prepare(bool splitSub, bool isEncodeAudio, bool isTsreplace);

    time_t getFirstFrameTime() const;

    // 2. ニコニコ実況コメントを取得したら呼ぶ
    void SetNicoJKList(const std::array<std::vector<NicoJKLine>, NICOJK_MAX>& nicoJKList);

    // 2. 各中間映像ファイルのCM解析後に呼ぶ
    // cmzones: CMゾーン（フィルタ入力フレーム番号）
    // divs: 分割ポイントリスト（フィルタ入力フレーム番号）
    void applyCMZones(int videoFileIndex, const std::vector<EncoderZone>& cmzones, const std::vector<int>& divs);

    // 3. CM解析が終了したらエンコード前に呼ぶ
    // cmtypes: 出力するCMタイプリスト
    AudioDiffInfo genAudio(const std::vector<CMType>& cmtypes);

    // 中間映像ファイルの個数
    int getNumVideoFile() const;

    // 入力映像規格
    VIDEO_STREAM_FORMAT getVideoStreamFormat() const;

    // PMT変更PTSリスト
    std::vector<int> getPidChangedList(int videoFileIndex) const;

    int getMainVideoFileIndex() const;

    // フィルタ入力映像フレーム
    const std::vector<FilterSourceFrame>& getFilterSourceFrames(int videoFileIndex) const;

    // フィルタ入力音声フレーム
    const std::vector<FilterAudioFrame>& getFilterSourceAudioFrames(int videoFileIndex) const;

    // 出力ファイル情報
    const EncodeFileInput& getEncodeFile(EncodeFileKey key) const;

    // 中間一時ファイルごとの出力ファイル数
    int getNumEncoders(int videoFileIndex) const;

    // 合計出力ファイル数
    //int getNumOutFiles() const {
    //	return (int)fileFormatId_.size();
    //}

    // video frame index -> VideoFrameInfo
    const VideoFrameInfo& getVideoFrameInfo(int frameIndex) const;

    // video frame index (DTS順) -> encoder index
    int getEncoderIndex(int frameIndex) const;

    // keyはvideo,formatの2つしか使われない
    const OutVideoFormat& getFormat(EncodeFileKey key) const;

    // genAudio後使用可能
    const std::vector<EncodeFileKey>& getOutFileKeys() const;

    // 映像データサイズ（バイト）、時間（タイムスタンプ）のペア
    std::pair<int64_t, double> getSrcVideoInfo(int videoFileIndex) const;

    // TODO: VFR用タイムコード取得
    // infps: フィルタ入力のFPS
    // outpfs: フィルタ出力のFPS
    void getTimeCode(
        int encoderIndex, int videoFileIndex, CMType cmtype, double infps, double outfps) const;

    const std::vector<int64_t>& getAudioFileOffsets() const;

    bool isVFR() const;

    bool hasRFF() const;

    double getInDuration() const;

    std::pair<double, double> getInOutDuration() const;

    // 音声フレーム番号リストからFilterAudioFrameリストに変換
    std::vector<FilterAudioFrame> getWaveInput(const std::vector<int>& frameList) const;

    void printOutputMapping(std::function<tstring(EncodeFileKey)> getFileName) const;

    // 以下デバッグ用 //

    void serialize(const tstring& path);

    void serialize(const File& file);

    static StreamReformInfo deserialize(AMTContext& ctx, const tstring& path);

    static StreamReformInfo deserialize(AMTContext& ctx, const File& file);

private:

    struct CaptionDuration {
        double startPTS, endPTS;
    };

    // 主要インデックスの説明
    // DTS順: 全映像フレームをDTS順で並べたときのインデックス
    // PTS順: 全映像フレームをPTS順で並べたときのインデックス
    // 中間映像ファイル順: 中間映像ファイルのインデックス(=video)
    // フォーマット順: 全フォーマットのインデックス
    // フォーマット(出力)順: 基本的にフォーマットと同じだが、「メイン以外は結合しない」場合、
    //                     メイン以外が分離されて異なるインデックスになっている(=format)
    // 出力ファイル順: EncodeFileKeyで識別される出力ファイルのインデックス

    // 入力解析の出力
    int numVideoFile_;
    std::vector<FileVideoFrameInfo> videoFrameList_; // [DTS順] 
    std::vector<FileAudioFrameInfo> audioFrameList_;
    std::vector<CaptionItem> captionItemList_;
    std::vector<StreamEvent> streamEventList_;
    std::vector<TimeInfo> timeList_;

    std::array<std::vector<NicoJKLine>, NICOJK_MAX> nicoJKList_;
    bool isEncodeAudio_;
    bool isTsreplace_;

    // 計算データ
    bool isVFR_;
    bool hasRFF_;
    std::vector<double> modifiedPTS_; // [DTS順] ラップアラウンドしないPTS
    std::vector<double> modifiedAudioPTS_; // ラップアラウンドしないPTS
    std::vector<double> modifiedCaptionPTS_; // ラップアラウンドしないPTS
    std::vector<double> audioFrameDuration_; // 各音声フレームの時間
    std::vector<int> ordredVideoFrame_; // [PTS順] -> [DTS順] 変換
    std::vector<double> dataPTS_; // [DTS順] 映像フレームのストリーム上での位置とPTSの関連付け
    std::vector<double> streamEventPTS_;
    std::vector<CaptionDuration> captionDuration_;

    std::vector<std::vector<int>> indexAudioFrameList_; // 音声インデックスごとのフレームリスト

    std::vector<OutVideoFormat> format_; // [フォーマット順]
    // 中間映像ファイルごとのフォーマット開始インデックス
    // サイズは中間映像ファイル数+1
    std::vector<int> formatStartIndex_; // [中間映像ファイル順]

    std::vector<int> fileFormatId_; // [フォーマット(出力)順] -> [フォーマット順] 変換
    // 中間映像ファイルごとのファイル開始インデックス
    // サイズは中間映像ファイル数+1
    std::vector<int> fileFormatStartIndex_; // [中間映像ファイル順] -> [フォーマット(出力)順]

    // 中間映像ファイルごと
    std::vector<std::vector<FilterSourceFrame>> filterFrameList_; // [PTS順]
    std::vector<std::vector<FilterAudioFrame>> filterAudioFrameList_;
    std::vector<int64_t> filterSrcSize_;
    std::vector<double> filterSrcDuration_;
    std::vector<std::vector<int>> fileDivs_; // CM解析結果

    std::vector<int> frameFormatId_; // [DTS順] -> [フォーマット(出力)順]

    // 出力ファイルリスト
    std::vector<EncodeFileKey> outFileKeys_; // [出力ファイル順]
    std::map<int, EncodeFileInput> outFiles_; // キーはEncodeFileKey.key()

    // 最初の映像フレームの時刻(UNIX時間)
    time_t firstFrameTime_;

    std::vector<int64_t> audioFileOffsets_; // 音声ファイルキャッシュ用

    double srcTotalDuration_;
    double outTotalDuration_;

    void reformMain(bool splitSub);

    void calcSizeAndTime(const std::vector<CMType>& cmtypes);

    template<typename I>
    void makeModifiedPTS(int64_t modifiedFirstPTS, std::vector<double>& modifiedPTS, const std::vector<I>& frames) {
        // 前後のフレームのPTSに6時間以上のずれがあると正しく処理できない
        if (frames.size() == 0) return;

        // ラップアラウンドしないPTSを生成
        modifiedPTS.resize(frames.size());
        int64_t prevPTS = modifiedFirstPTS;
        for (int i = 0; i < int(frames.size()); ++i) {
            int64_t PTS = frames[i].PTS;
            if (PTS == -1) {
                // PTSがない
                THROWF(FormatException,
                    "PTSがありません。処理できません。 %dフレーム目", i);
            }
            int64_t modPTS = prevPTS + int64_t((int32_t(PTS) - int32_t(prevPTS)));
            modifiedPTS[i] = (double)modPTS;
            prevPTS = modPTS;
        }

        // ストリームが戻っている場合は処理できないのでエラーとする
        for (int i = 1; i < int(frames.size()); ++i) {
            if (modifiedPTS[i] - modifiedPTS[i - 1] < -60 * MPEG_CLOCK_HZ) {
                // 1分以上戻っている
                ctx.incrementCounter(AMT_ERR_NON_CONTINUOUS_PTS);
                ctx.warnF("PTSが戻っています。正しく処理できないかもしれません。 [%d] %.0f -> %.0f",
                    i, modifiedPTS[i - 1], modifiedPTS[i]);
            }
        }
    }

    void registerOrGetFormat(OutVideoFormat& format);

    bool isEquealFormat(const OutVideoFormat& a, const OutVideoFormat& b);

    struct AudioState {
        double time = 0; // 追加された音声フレームの合計時間
        double lostPts = -1; // 同期ポイントを見失ったPTS（表示用）
        int lastFrame = -1;
    };

    struct OutFileState {
        int formatId; // デバッグ出力用
        double time; // 追加された映像フレームの合計時間
        std::vector<AudioState> audioState;
        FileAudioFrameList audioFrameList;
    };

    AudioDiffInfo initAudioDiffInfo();

    // フィルタ入力から音声構築
    AudioDiffInfo genAudioStream();

    void genWaveAudioStream();

    // ソースフレームの表示時間
    // index, nextIndex: DTS順
    double getSourceFrameDuration(int index, int nextIndex);

    void addVideoFrame(OutFileState& file,
        const std::vector<AudioFormat>& audioFormat,
        double pts, double duration, AudioDiffInfo* adiff);

    void fillAudioFrames(
        OutFileState& file, int index, // 対象ファイルと音声インデックス
        const AudioFormat* format, // 音声フォーマット
        double pts, double duration, // 開始修正PTSと90kHzでのタイムスパン
        AudioDiffInfo* adiff);

    // lastFrameから順番に見て音声フレームを入れる
    void fillAudioFramesInOrder(
        OutFileState& file, int index, // 対象ファイルと音声インデックス
        const AudioFormat* format, // 音声フォーマット
        double& pts, double& duration, // 開始修正PTSと90kHzでのタイムスパン
        AudioDiffInfo* adiff);

    // ファイル全体での時間
    std::pair<int, double> elapsedTime(double modPTS) const;

    void genCaptionStream();
};


