#pragma once

#include "common.h"
#include "StreamUtils.h"
#include "TranscodeSetting.h"
#include "ProcessThread.h"
#include "FileUtils.h"

// Whisper字幕生成の起動/ログ取りまとめ
class SubtitleGenerator : public AMTObject {
public:
    explicit SubtitleGenerator(AMTContext& ctx);

    // Whisperコマンドを実行し、JSON字幕を生成する
    //  - audioPath: 入力の一時WAV/音声ファイル
    //  - outDir: 出力ディレクトリ (settings.getTmpWhisperDir())
    //  - outJsonPath: 出力JSONファイルパス
    //  - extraOptions: 追加オプション (空可)
    //  - isUtf8Log: whisperの出力がUTF-8想定ならtrue
    void runWhisper(const tstring& whisperPath,
                    const tstring& audioPath,
                    const tstring& outDir,
                    const tstring& extraOptions,
                    bool isUtf8Log = true);
};
