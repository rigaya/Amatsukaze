# バッチファイル実行機能

Amatsukazeでは下記タイミングでバッチファイルを実行できます。

batフォルダに指定のプリフィックスを付けてバッチファイルを置くと、GUIから選択できるようになります。

| 実行タイミング | プリフィックス | 設定箇所 | 実行主体 |
|:--|:--|:--|:--|
| TSファイル追加時 | 追加時_       | TSファイルドロップ画面 | AmatsukazeServer |
| 実行前           | 実行前_       | プロファイル           | AmatsukazeServer |
| エンコード前     | エンコード前_ | プロファイル           | AmatsukazeCLI    |
| 実行後           | 実行後_       | プロファイル           | AmatsukazeServer |
| キュー完了後     | キュー完了後_  | 基本設定              | AmatsukazeServer |

## 使用可能な環境変数

実行タイミングにより、使用可能な変数は異なるので注意してください。

### TSファイル追加時、実行前、実行後で有効

| 変数名 | 説明 | 出力例 |
|:--|:--|:--|
| ITEM_ID | アイテムに一意に振られるID。追加時、実行前、実行後で同じアイテムを追跡できる。Amatsukazeを再起動するとIDが変わるので注意。 | `1` |
| IN_PATH | 入力ファイルパス | `Y:\キャプチャ\succeeded\サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX).ts` |
| IN_PATH2 | 2番目の入力ファイルパス | `Y:\キャプチャ\succeeded\サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX).ts` |
| IN_PATH_ZTOH | 全角→半角変換された入力ファイルパス | `Y:\キャプチャ\succeeded\サイレント・ウィッチ 沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX).ts` |
| IN_PATH2_ZTOH | 全角→半角変換された2番目の入力ファイルパス | `Y:\キャプチャ\succeeded\サイレント・ウィッチ 沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX).ts` |
| IN_FILENAME | 入力ファイル名 | `サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| IN_FILENAME_ZTOH | 全角→半角変換された入力ファイル名 | `サイレント・ウィッチ 沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| IN_FILENAME2 | 2番目の入力ファイル名 | `サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| IN_FILENAME2_ZTOH | 全角→半角変換された2番目の入力ファイル名 | `サイレント・ウィッチ 沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| IN_DIR | 入力ディレクトリ | `Y:\キャプチャ\succeeded` |
| IN_EXT | 入力ファイル拡張子 | `.ts` |
| OUT_PATH | 出力ファイルパス（拡張子を含まない） | `Y:\キャプチャ\encoded\サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| OUT_PATH2 | 2番目の出力ファイルパス | `Y:\キャプチャ\encoded\サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| OUT_PATH_ZTOH | 全角→半角変換された出力ファイルパス | `Y:\キャプチャ\encoded\サイレント・ウィッチ 沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| OUT_PATH2_ZTOH | 全角→半角変換された2番目の出力ファイルパス | `Y:\キャプチャ\encoded\サイレント・ウィッチ 沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| OUT_FILENAME | 出力ファイル名 | `サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| OUT_FILENAME_ZTOH | 全角→半角変換された出力ファイル名 | `サイレント・ウィッチ 沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| OUT_FILENAME2 | 2番目の出力ファイル名 | `サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| OUT_FILENAME2_ZTOH | 全角→半角変換された2番目の出力ファイル名 | `サイレント・ウィッチ 沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX)` |
| OUT_DIR | 出力ディレクトリ | `Y:\キャプチャ\encoded` |
| SERVICE_ID | サービスID（チャンネルID） | `23608` |
| SERVICE_NAME | サービス名（チャンネル名） | `ＴＯＫＹＯ　ＭＸ１` |
| SERVICE_NAME_ZTOH | 全角→半角変換されたサービス名 | `TOKYO MX1` |
| TS_TIME | TSファイルの時刻 | `2025/07/05 0:15:17` |
| ITEM_MODE | アイテムモード | `Batch` |
| ITEM_PRIORITY | アイテム優先度(1-5) | `3` |
| EVENT_GENRE | 番組ジャンル | `アニメ／特撮 - 国内アニメ` |
| IMAGE_WIDTH | 映像幅 | `1440` |
| IMAGE_HEIGHT | 映像高さ | `1080` |
| EVENT_NAME | 番組名 | `[新]サイレント・ウィッチ　沈黙の魔女の隠しごと　＃１「同期が来たりて無茶を言う」` |
| EVENT_NAME2 | 2番目の番組名 | `サイレント・ウィッチ　沈黙の魔女の隠しごと　＃１「同期が来たりて無茶を言う」` |
| EVENT_NAME_ZTOH | 全角→半角変換された番組名 | `[新]サイレント・ウィッチ 沈黙の魔女の隠しごと #1「同期が来たりて無茶を言う」` |
| EVENT_NAME2_ZTOH | 全角→半角変換された2番目の番組名 | `サイレント・ウィッチ 沈黙の魔女の隠しごと #1「同期が来たりて無茶を言う」` |
| TAG | タグ（セミコロン区切り） | （空） |
| IN_PIPE_HANDLE | 入力パイプハンドル | （実行時のみ設定） |
| OUT_PIPE_HANDLE | 出力パイプハンドル | （実行時のみ設定） |

### 実行前および実行後のみで有効

| 変数名 | 説明 | 出力例 |
|:--|:--|:--|
| PROFILE_NAME | プロファイル名 | `QSVEnc-test` |

### 実行後のみで有効

| 変数名 | 説明 | 出力例 |
|:--|:--|:--|
| SUCCESS | 成功=1,失敗=0 | `1` |
| ERROR_MESSAGE | エラーメッセージ（失敗したときのみ） | （空） |
| IN_DURATION | 入力ファイルの再生時間 | `1806.405` |
| OUT_DURATION | 出力ファイルの再生時間 | `1806.405` |
| IN_SIZE | 入力ファイルのサイズ（バイト単位） | `2321108348` |
| OUT_SIZE | 出力ファイルのサイズ（バイト単位） | `600557423` |
| LOGO_FILE | ロゴファイルパス | `SID23608-1.lgd` |
| NUM_INCIDENT | インシデント数 | `0` |
| JSON_PATH | 出力JSONパス | `C:\ProgramEx\Amatsukaze\data\logs\2025-09-19_102809.387.json` |
| LOG_PATH | ログファイルパス | `C:\ProgramEx\Amatsukaze\data\logs\2025-09-19_102809.387.log` |

### エンコード前で有効

| 変数名 | 説明 | 出力例 |
|:--|:--|:--|
| CLI_IN_PATH | CLI用入力ファイルパス（設定されたソースファイルパス） | `Y:/キャプチャ/サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX).ts` |
| TS_IN_PATH | 元のTSファイルパス（オリジナルソースファイルパス） | `Y:/キャプチャ/サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX).ts` |
| TS_IN_DIR | 元のTSファイルのディレクトリ | `Y:/キャプチャ` |
| CLI_OUT_DIR | CLI用出力ディレクトリ | `Y:/キャプチャ/encoded` |
| OUT_DIR | 出力ディレクトリ | `Y:/キャプチャ/encoded` |
| SERVICE_ID | サービスID（チャンネルID） | `23608` |
| CLI_OUT_PATH | CLI用出力ファイルパス | `Y:/キャプチャ/encoded/サイレント・ウィッチ　沈黙の魔女の隠しごと #01 「同期が来たりて無茶を言う」 (MX).mp4` |
| IMAGE_WIDTH | 映像幅 | `1440` |
| IMAGE_HEIGHT | 映像高さ | `1080` |
| AMT_ENCODER | 使用エンコーダー名 | `QSVEnc` |
| AMT_AUDIO_ENCODER | 使用音声エンコーダー名 | `none` |
| AMT_TEMP_DIR | 一時作業ディレクトリ | `Y:/Temp/amt13416255` |
| AMT_TEMP_AVS | AviSynthスクリプト一時ファイル | `Y:/Temp/amt13416255/v0-1-0-main.avstmp` |
| AMT_TEMP_AVS_TC | AviSynthタイムコードファイル | `Y:/Temp/amt13416255/v0-1-0-main.avstmp.timecode.txt` |
| AMT_TEMP_AVS_DURATION | AviSynth再生時間ファイル | `Y:/Temp/amt13416255/v0-1-0-main.avstmp.duration.txt` |
| AMT_TEMP_AFS_TC | AFSタイムコードファイル | `Y:/Temp/amt13416255/v0-1-0-main.timecode.txt` |
| AMT_TEMP_VIDEO | エンコード動画一時ファイル | `Y:/Temp/amt13416255/v0-1-0-main.raw` |
| AMT_TEMP_AUDIO | 音声一時ファイル（メイン） | `Y:/Temp/amt13416255/a0-1-0-0-main.aac` |
| AMT_TEMP_AUDIO_0 | 音声一時ファイル（トラック0） | `Y:/Temp/amt13416255/a0-1-0-0-main.aac` |
| AMT_TEMP_AUDIO_1 | 音声一時ファイル（トラック1） | `Y:/Temp/amt13416255/a0-1-0-1-main.aac` |
| AMT_TEMP_CHAPTER | チャプター情報一時ファイル | `Y:/Temp/amt13416255/chapter0-1-0-main.txt` |
| AMT_TEMP_TIMECODE | タイムコード一時ファイル | `Y:/Temp/amt13416255/v0-1-0-main.avstmp.timecode.txt` |
| AMT_TEMP_ASS | ASS字幕一時ファイル（メイン） | `Y:/Temp/amt13416255/c0-1-0-0-main.ass` |
| AMT_TEMP_ASS_0 | ASS字幕一時ファイル（トラック0） | `Y:/Temp/amt13416255/c0-1-0-0-main.ass` |
| AMT_TEMP_ASS_1 | ASS字幕一時ファイル（トラック1） | `Y:/Temp/amt13416255/c0-1-0-1-main.ass` |
| AMT_TEMP_SRT | SRT字幕一時ファイル（メイン） | `Y:/Temp/amt13416255/c0-1-0-0-main.srt` |
| AMT_TEMP_SRT_0 | SRT字幕一時ファイル（トラック0） | `Y:/Temp/amt13416255/c0-1-0-0-main.srt` |
| AMT_TEMP_SRT_1 | SRT字幕一時ファイル（トラック1） | `Y:/Temp/amt13416255/c0-1-0-1-main.srt` |
| AMT_TEMP_ASS_NICOJK_720S | NicoJK ASS字幕（720p ステレオ） | `Y:/Temp/amt13416255/nicojk0-1-0-main-720S.ass` |
| AMT_TEMP_ASS_NICOJK_720T | NicoJK ASS字幕（720p トルーサラウンド） | `Y:/Temp/amt13416255/nicojk0-1-0-main-720T.ass` |
| AMT_TEMP_ASS_NICOJK_1080S | NicoJK ASS字幕（1080p ステレオ） | `Y:/Temp/amt13416255/nicojk0-1-0-main-1080S.ass` |
| AMT_TEMP_ASS_NICOJK_1080T | NicoJK ASS字幕（1080p トルーサラウンド） | `Y:/Temp/amt13416255/nicojk0-1-0-main-1080T.ass` |

## バッチファイルから使える専用コマンド

- AddTag <タグ名>
  - 追加時/実行前/実行後で使用可能
  - タグを追加。また、現在設定されているタグを標準出力に出力。（複数ある場合は";"(セミコン)区切り）
    同じタグは重複できないので、既に存在するタグを追加しようとしても無視される。
    引数なしで実行すると、タグ追加は行わず、現在設定されているタグだけ出力。
- SetOutDir <出力フォルダ>
  - 追加時/実行前で使用可能
  - 出力先を変更
- SetPriority <優先度>
  - 追加時で使用可能
  - 優先度(1～5)を変更
- CancelItem
  - 追加時/実行前で使用可能
  - キャンセル
- GetOutFiles <オプション>
  - 実行後で使用可能
  - 出力ファイルリストを取得（複数ある場合は";"(セミコロン)区切り）
  -オプションは下表の項目の組み合わせ、または、all
        例) メイン動画と対応する字幕とログファイルを取得: GetOutFiles vsl

    | オプション | 説明 |
    |:--|:--|
    | v        | メインの出力動画ファイル（１つだけ） |
    | c        | メインの出力に対応するCM部分のファイル |
    | s        | メインの出力に対応する字幕ファイル |
    | w        | メイン以外の出力動画ファイル |
    | d        | メイン以外の出力に対応するCM部分のファイル |
    | t        | メイン以外の出力に対応する字幕ファイル |
    | r        | EDCB関連ファイル |
    | l        | ログファイル |