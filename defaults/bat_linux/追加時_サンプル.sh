#!/bin/bash

# 字幕の有無を確認し、字幕がある場合はタグを追加
subtitle_result=$(AmatsukazeCLI -i "$IN_PATH" -s $SERVICE_ID --mode probe_subtitles)
if [ "$subtitle_result" = "字幕あり" ]; then
    AddTag 字幕
fi

# 音声情報を確認し、多音声の場合はタグを追加
audio_result=$(AmatsukazeCLI -i "$IN_PATH" -s $SERVICE_ID --mode probe_audio | awk '{print $2}')
if [ "$audio_result" != "1" ]; then
    AddTag 多音声
fi
