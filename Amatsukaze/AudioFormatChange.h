#pragma once

// 音声フォーマット変更時の出力方法。CLIの既定値は従来互換の分割。
enum AUDIO_FORMAT_CHANGE_MODE {
    AFC_SPLIT = 0,
    AFC_MERGE = 1,
    AFC_SEPARATE = 2
};
