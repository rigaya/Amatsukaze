#pragma once

#include "common.h"
#include "StreamUtils.h"
#include "TranscodeSetting.h"
#include "ProcessThread.h"
#include "FileUtils.h"

// Whisperプロセス起動用パラメータ
struct WhisperProcessParam {
    tstring whisperPath;
    tstring audioPath;
    tstring outDir;
    tstring outFileWithoutExt;
    tstring extraOptions;
    bool enableVtt;
    bool isUtf8Log;
    bool captureOnly;

    WhisperProcessParam()
        : enableVtt(false)
        , isUtf8Log(true)
        , captureOnly(false) {}
};

// Whisper字幕生成の起動/ログ取りまとめ
class SubtitleGenerator : public AMTObject {
public:
    explicit SubtitleGenerator(AMTContext& ctx);

    // Whisperコマンドを実行し、字幕を生成する (従来形式)
    void runWhisper(const tstring& whisperPath,
                    const tstring& audioPath,
                    const tstring& outDir,
                    const tstring& outFileWithoutExt,
                    const tstring& extraOptions,
                    bool enableVtt,
                    bool isUtf8Log = true);

    // Whisperコマンドを実行し、字幕を生成する (構造体で一括指定)
    void runWhisper(const WhisperProcessParam& param);

    // Whisperプロセスを起動し、そのStdRedirectedSubProcessを返す
    //  - captureOnly == true の場合、標準出力/標準エラーはコンソールに直接出さず
    //    getCapturedLines() 経由でのみ取得できる
    std::unique_ptr<StdRedirectedSubProcess> startWhisperProcess(const WhisperProcessParam& param);
};


bool exeIsWhisperCpp(const tstring& whisperPath);
