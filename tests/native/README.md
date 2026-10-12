# AmatsukazeNativeTests

`AmatsukazeNativeTests` は、外部ファイル、ネットワーク、GPU、GoogleTest に依存しない公開ネイティブ単体テストです。対象ロジックは本体ライブラリから呼び出します。C++ コンテナをDLL境界で渡すケースだけ、テスト専用の C ABI ラッパーを使います。Release ビルドでも無効化されない明示比較を行います。

## 実行方法

Linux:

```bash
meson setup build-native --buildtype release
meson compile -C build-native AmatsukazeNativeTests
meson test -C build-native --suite native --print-errorlogs
```

Windows:

```bat
tests\native\build_windows.cmd
```

利用可能なケースは `--list`、選択実行は `--select <ケース名>` を使います。複数の `--select` を指定できます。指定名の一つでも未登録なら終了コード2、ケース失敗は1です。

## 旧テストからの移行対応表

| 旧 GoogleTest ケース | 移行先 | 扱い |
|---|---|---|
| `CaptionText.UnicodeCharacterFormatting` | `caption_text_length` | 公開単体テストへ移行 |
| `VfrZonesBug` の通常ゾーン計算 | `bitrate_zones` | 公開単体テストへ移行。外部入力を読む再現データ部分は非公開へ移管 |
| VFR入力判定の実装内ケース | `vfr_input_detection` | 公開単体テストへ移行 |
| `CRC.*` | `crc32` | 公開単体テストへ移行。CRC-32/MPEG-2の検査値とPSIセクション検査で確認 |
| `Util.readOpt`, `Util.AutoBufferTest` | `bit_reader`, `auto_buffer` | 公開単体テストへ移行。旧テストが呼ぶ `--mode test_read_bits` は値を表示するだけで合否を判定しないため、BitReader/BitWriter/AutoBuffer を直接検証 |
| `MPEG2Parser`, `H264Parser`, `H264Parser1Seg`, `Pulldown`, `LargeTsParse`, `MPEG2PSVerifier` | 非公開コンポーネントテスト | 外部の映像・TS・MPEG-PS素材に依存 |
| `AacDecodeVerifyTest`, `WaveWriter`, `SplitDualMonoAAC`, `AACDecodeTest` | 非公開コンポーネントテスト | 外部音声素材または出力検証が必要 |
| `encodeMpeg2Test`, `fileStreamInfoTest`, `DamemojiTest`, `LosslessTest`, `LogoFrameTest`, `CaptionASSTest` | 非公開コンポーネント/統合テスト | 外部素材、AviSynth、ロゴまたは出力ファイルに依存 |
| `Process.SimpleProcessTest` | 非公開コンポーネントテスト | 子プロセス実行を伴う |
| `EncoderOptionTest01`～`EncoderOptionTest09` | `encoder_option` | 公開単体テストへ移行。旧テストの `--mode test_eo` は解析結果を表示するだけで合否を判定しないため、テスト専用の C ABI ラッパー `ParseEncoderOptionForTest` を通して期待値と比較する |
| `AudioTrack.SeparatePlanner` | `audio_track_separate_planner` | 公開単体テストへ移行。`PlanSeparateAudioTracks`/`ValidateAudioTrackPlans` はSTLコンテナを受け渡すため、テスト専用のC ABIラッパー `PlanAudioTracksForTest`/`ValidateAudioTrackPlansForTest` を通して、旧テストの期待値と比較する |
| `AudioTrack.MergePlanner` | `audio_track_merge_planner` | 公開単体テストへ移行。分離と同じC ABIラッパーで `PlanMergeAudioTracks` を検証する |
| `AudioTrack.SilentAdtsBytes` | `audio_silent_adts_bytes` | 公開単体テストへ移行。設計書の手組みバイト列との一致と拒否を `GenerateSilentAdtsFrameForTest` 経由で検証する。旧テストの `GetAudioSamplingFrequencyIndex` の逆引きは、分離プランのサンプリング周波数インデックスと未対応レートの拒否で確認する。PES経由の並行処理は既存の `audio_pes_pipeline` が担当 |
| `AudioTrackConvertTest.cpp` | `audio_channel_conversion` | `ConvertAudioChannels` の素材不要な検証部分だけ公開単体テストへ移行 (テスト専用C ABI `ConvertAudioChannelsForTest`)。旧テストはモノラル⇔5.1chの変換を拒否する前提だが、現行実装はステレオ経由で変換するため、現行仕様に合わせた。入力ADTSからAACを再構築する部分と `run_audio_track_convert_test.py`、`run_audio_track_seam_test.py` は外部音声素材と`ffmpeg`CLIに依存する実験用スタンドアロンで、非公開コンポーネントテスト |
| `AudioTrackStreamReformTest.cpp`, `AudioTrackFaadSilenceTest.cpp`, `run_audio_track_test.py` | 非公開コンポーネントテスト | `StreamReform` 全体、libfaad、出力ディレクトリのファイルを使う実験用スタンドアロンで、`compile_commands.json` から単体ビルドする。公開単体テストの対象外 |
| `CLI.ArgumentTest` | `command_line_arguments` | 公開単体テストへ移行。旧テストの `--mode test_parseargs` は例外が出ないことしか見ていないため、`AmatsukazeCLI.hpp` の static `parseArgs` の結果を、同じ翻訳単位で定義したテスト専用C ABI `ParseCommandLineForTest` が「キー=値」の改行区切りテキストへ直列化して返し、呼出側バッファ経由で値を比較する。旧テストの3パターン (10進と`0x6308`のサービスID、未知オプション`--ourput`の拒否) に加え、全オプション指定時の入出力パス・エンコーダ種別・オプション・muxer・timelineeditor・JSON・dumpの値、パス区切りの`/`への正規化、既知拡張子 (`.mp4`/`.mkv`/`.m2ts`/`.ts`) だけが除かれる出力パス、最小構成の既定値、モード別の必須引数、値が欠けた末尾オプション、整数オプションの厳密な解析 (空、`12abc`、`1.5`、intの範囲外、不正な16進を引数の拒否として扱い、サービスIDは`0x`と`0X`の両方を16進として受理する)、種別を表す値 (エンコーダ、音声エンコーダ、デコーダ) の不明な名前の拒否とサーバーが渡す綴りの受理、範囲外の値、実況マスクの有効化条件を確認する。実行ファイル位置やPATH探索に依存する既定の実行ファイル名は比較せず、比較する実行ファイルは存在しない絶対パスで明示指定する。作業ディレクトリは取得に一時ディレクトリの作成を要するため比較しない |
| `DecodePerformance` | 非公開ベンチマーク | 性能計測であり単体テストの合否に含めない |

既存の `AmatsukazeUnitTest/` と GoogleTest は、この移行段階では削除しません。C# の `AmatsukazeServerTest` xUnit は維持し、ネットワークを使う任意テストは `Category=Network` としてオフラインの標準単体テストから分離します。標準群は次で実行します。

```bash
dotnet test AmatsukazeServerTest/AmatsukazeServerTest.csproj --filter "Category!=Network"
```

`bitrate_zones` の入力はフレーム数+1個の終端時刻を含める。8フレーム単位で計算した期待値は `0-40: 2.5`、`40-64: 1.35`、`64-128: 1.1375`、`128-150: 2.0` である。第2・第3ゾーンの値はそれぞれ `(1.5+1.5+1.05)/3` と `(0.6+0.6+1+1+1.5+2+1.2+1.2)/8` から独立に計算できる。19単位のコスト上限は `19*0.15=2.85` で、5ゾーン時の累積値 `2.366666...` に最小併合コスト `0.8` を加えた4ゾーン時は `3.166666...` になる。実装は追加前の累積値で継続可否を判定するため、次の反復で停止し、4→3の併合コスト `0.927272...` は適用されない。旧テストの `40-128: 約1.195` はこの未実施の併合を前提とするため移植しない。

## 素材依存だった旧ケースを置き換えた公開テスト

次のケースは、外部の素材を使う旧ケース (表示するだけ、または例外が出ないことしか見ていなかったもの) の検証したかった観点を、テスト内で生成した入力と明示比較に置き換えたものである。旧ケースの移行対応表の行と監査 manifest への反映は、非公開側の移管状況とあわせて別途行う。

- `wave_header` (旧 `WaveWriter`): `writeWaveHeader` が書く44バイトの全フィールドを、仕様から求めた期待値と比較する。16bitステレオ48kHz、8bitモノラル、24bit 5.1ch、サンプル数0、全体が2^32-1バイトちょうどの上限と1バイト超過、不正なチャンネル数・サンプルレート・ビット数・負のサンプル数の拒否、小さい出力バッファを確認する。テスト専用C ABI `WriteWaveHeaderForTest` は、無名の一時ファイル (`tmpfile`、閉じると自動削除) に書いたヘッダを読み戻して返す。このテストの追加で、旧実装の不具合 (ブロックアラインが `channels * bitswidth` でバイト換算されていない、量子化ビット数が常に16、サイズ欄の符号付き型と上限2^32-1のコメント不一致) を修正した。
- `audio_silent_adts_decode` (旧 `AudioTrackFaadSilenceTest.cpp`、`AACDecodeTest` の復号エラー検出): `GenerateSilentAdtsFrame` で作る6種類 (1〜5.1ch) の無音ADTSを、同梱のFAAD (16bit整数) とlibavcodecのAAC復号器 (浮動小数) の両方で復号し、全サンプルが0であること、チャンネル数 (FAADはモノラルをPS対応で2chへ展開する)、サンプル数が1024の整数倍で全フレーム数から1フレーム分以内であることを確認する。復号はすべて本体ライブラリ内の `DecodeSilentAdtsForTest` で行うので、テスト実行ファイルにlibfaadやlibavcodecを直接リンクしない (meson・vcxprojの依存の追加なし)。
- `dual_mono_splitter` (旧 `SplitDualMonoAAC`): PCEで2つのSCEを宣言する `channel_configuration=0` の無音ADTS (デュアルモノ) をテスト内で組み立て、`DualMonoSplitter` に3回入力し、主音声 (タグ0) と副音声 (タグ1) それぞれが、独立に組み立てた期待値 (MPEG-2のID、`channel_configuration=1`、元のエレメントのタグを保つ) とバイト単位で一致すること、ADTSでないデータとデュアルモノでないステレオフレームの拒否、小さい出力バッファを確認する。テスト専用C ABIは `SplitDualMonoAdtsForTest`。
- `bitrate_zones_properties` (旧 `VfrZonesBug`): 元の再現データ (`zone_param.dat`) は手元にないので、固定seedの乱数で作ったタイムコードとCMゾーン (1〜200000フレーム、24/30/60fps、変化の細かさ違い) で `MakeVFRBitrateZones` の不変条件を確認する。ゾーンが先頭0から末尾まで隙間なく覆うこと、境界が8フレームの倍数であること、ゾーン数が上限内であること、各ゾーンのビットレートが、フレーム間隔・時間係数・CMゾーンの補正から独立に求めた8フレーム単位のビットレートの平均と一致すること。既存の `bitrate_zones` は固定値の期待値、こちらは性質を見る。
- `ps_writer_round_trip` (旧 `MPEG2PSVerifier`): 映像 (32000バイトを超えて分割されるPESと、33bit境界直前のPTSを含む) と音声のPESを `PsStreamWriter` に書かせ、`PsStreamVerifier` が受理することと、テスト内の独立したPS解析 (パックヘッダのマーカーとSCRの単調性、プログラムストリームマップのCRCと内容、PESのPTS/DTS、分割されたペイロードの結合、終了コード) の結果が入力と一致することを確認する。テスト専用C ABIは `WritePsStreamForTest`。

## StreamReformと音声トラック構築の合成入力テスト

旧 `AudioTrackStreamReformTest.cpp` (単体ビルドの実験用プログラム) の `Require` による判定を、期待値ごと次のケースへ移した。入力はすべてテスト内または本体ライブラリ内で合成し、素材は使わない。ファイルが必要なケースは `std::filesystem::temp_directory_path()` の下に専用の一時ディレクトリを作り、終了時に必ず削除する。

テスト専用C ABIは2本で、どちらも合否を出さず、結果の要約だけを返す。判定と期待値はテスト側にある。
- `RunAudioReformScenarioForTest`: 旧テストの `RunScenario` の入力生成 (映像384フレーム、音声600フレーム相当、トラック0は2ch→5.1chの切替と欠落、トラック1は2回の出現、PTS・イベント列) とフォールバック用の音声ファイル生成を本体側に置く。`StreamReformInfo` の構築から `prepare`、`applyCMZones`、`genAudio` までを実行し、出力ファイル数、音声トラック数、各トラックの参照列とPTS、プランの数・名前・フレーム数・CONVERT参照の有無、`ValidateAudioTrackPlans` の結果、無音参照のPCM入力を固定長の構造体と呼出側バッファで返す。`StreamReform` の入力 (映像・音声・字幕・イベントのSTLコンテナ) を一から C ABI にすると、DLL境界を越える型が多すぎるため、入力の生成側を本体に置いた。
- `BuildAudioTracksForTest`: テストが連結済みのADTSフレーム列、各フレームのレイアウト、入力トラックのフレーム番号列を渡すと、プラン作成 (分離または統合) と `BuildAudioTrack` による書き出しを行い、プランごとの状態 (成功、またはFormatExceptionの文面) と出力バイト列を返す。入力フレーム (無音、デュアルモノ、PCE付きステレオ) はテスト側で組み立てる。

- `audio_reform_separate`、`audio_reform_merge` (旧 `RunScenario` の `AFC_SEPARATE`・`AFC_MERGE` 各4本): 欠落フレーム数5と6 (欠落を無音で埋めるかを分ける閾値の前後)、CMカット (副音声の再出現区間だけが残る)、0.7フレームの位相ずれ (連続音声を再選択も欠落もせず1920ティック間隔で使う) について、音声変更でファイルが分割されないこと、途中で増えた音声トラックが保たれ全トラックの長さが揃うこと、副音声の先頭と末尾が無音、欠落・消失・再出現の位置、無音参照のPCM入力を確認する。統合では、論理トラック数とトラック名 (`Audio0`、`Audio1`)、異なるレイアウトがCONVERT参照になること、全プランの長さが参照列と一致することも確認する。
- `audio_reform_unsupported_fallback` (旧 `fallback` 1〜4): 周波数の混在、未対応レイアウト、AAC Main、PCE付きステレオの音声があると、ジョブ全体が従来のsplit (エンコーダ数が2以上) に戻ることを、分離・統合の両方で確認する。
- `audio_reform_cfg0_dual_mono` (旧 `fallback` 5): cfg0 (PCE) の2SCE音声が主副の2本のmonoトラックに展開され、プランが3本になることを確認する。
- `audio_builder_copy_silence` (旧 `BuilderCopyTest`)、`audio_builder_dual_mono` (旧 `BuilderDualMonoTest`)、`audio_builder_reject_pce` (旧 `BuilderRejectPceTest`)、`audio_builder_merge_convert` (旧 `BuilderMergeSilenceTest`): コピーと無音の結合が入力フレームとバイト単位で一致すること、デュアルモノがADTSパーサで判定され分離後のチャンネル構成が主mono・副mono・ステレオになること、cfg0のステレオがCOPYで「ADTSヘッダ」のエラーとして拒否されること、統合の目標レイアウト (ステレオ→5.1chのアップミックス、5.1ch→ステレオのダウンミックス) と出力のフレーム数・チャンネル構成を確認する。旧テストは統合変換で例外が出ないことと目標レイアウトしか見ていなかったので、出力のADTSフレーム数とチャンネル構成の確認を加えた。

`bitrate_zones_properties` には、映像の範囲外にはみ出すCMゾーン (開始が負、終了が映像末尾より後、完全に範囲外、空) の入力を加えた。`MakeVFRBitrateZones` は、これらでユニット配列の範囲外を読み書きしていたため、CMゾーンを映像の先頭と末尾で切り詰めるよう修正した。AddressSanitizerで修正前に `heap-buffer-overflow` を再現し、修正後に消えることを確認している。

## 既知の問題

- H.264パーサはSEIのbuffering_periodで得たDTSを次のアクセスユニットへ引き継いでいないため、PESにPTS/DTSがないフレームはSEIのpic_timingから時刻を補完できず-1のままになる。PESのPTSとSEIから求めた値の照合も実質的に行われない。修正すると照合が有効になり、実放送で警告と`AMT_ERR_H264_PTS_MISMATCH`が増える可能性があるため、実TSでの確認なしには修正しない。放送のH.264ではPESに常にPTSが付くため実害は小さい。

テストは TCHAR を原則とし、出力は `_ftprintf`、書式化は `strsprintf` を使う。`common` の静的ライブラリをリンクし、本体DLLでは非公開の `StringFormat`（`THROW` が使用）のため `Amatsukaze/StringUtils.cpp` もテストに組み込む。

Windows では本体とテストを同じ Visual C++ ツールセットおよび `/MT` ランタイムでビルドする。`bitrate_zones`、`vfr_input_detection`、`audio_track_*_planner`、`audio_channel_conversion`、`command_line_arguments`、`wave_header`、`dual_mono_splitter`、`ps_writer_round_trip`、`audio_reform_*`、`audio_builder_*` はテスト専用の C ABI ラッパーを通し、DLL境界で `std::vector` の所有権や実装を受け渡ししない。

`caption_pgs_canvas_size` は、出力映像のフレームサイズとSARからPGSのキャンバス (表示サイズ) を求める `CaptionPgsCanvasSize` を、テスト専用の C ABI `CaptionPgsCanvasSizeForTest` を通して検証する。HD (1440x1080 SAR 4:3 → 1920x1080)、SD 16:9 (720x480 SAR 32:27 → 853x480)、SARが1未満のSD 4:3 (高さを伸ばして720x540)、SAR不明、`--sar` の優先と片方だけの指定の無視、長辺4096を超える場合の比例縮小 (横長・縦長・8K) と、不正な引数を確認する。

## PGSエンコーダ単体テスト

`PgsEncoderTests`はPGS実装を直接コンパイルし、FFmpegの`pgssub`デコーダでSUPを往復検証する。本体DLLやlibaribcaptionに依存しない。Linuxでは`meson compile -C build-native PgsEncoderTests`と`meson test -C build-native PgsEncoderTests --print-errorlogs`で実行する。Windowsでは`build_windows.cmd`が専用`PgsEncoderTests.vcxproj`をビルドして実行する。

255色以下ではRGBAの各成分の誤差を±2以内とし、透明画素のRGBは評価対象外とする。4096色のRGBAグラデーションでは非透明画素のRGBA成分全体のPSNRを30dB以上とする。480/576/577/1080行で色変換の切替境界を検証し、領域結合、透明行列の削除、ODS分割、RLEの長さ境界と行末、PCS/WDS/ENDの固定バイト列、消去WDSと直前エポックのウィンドウ定義の一致、消去後の透明画像、隣接イベントの消去省略、255/256色の共有パレット境界、半透明のsource-over合成、不正入力、空・全透明イベント、PTSとcomposition番号の折り返し、ファイル出力とメモリ出力の一致も検証する。

AVX2は実行時にCPUとOSの対応を判定する。RLEは逐次参照とのバイト一致、パレットの最近傍色は従来のdouble演算順による全画素インデックス一致を検証する。複数イベントでは、逐次符号化との一致、隣接イベントの消去省略とcomposition番号、並列処理中の例外伝播を確認する。コンパイル時に`AMT_PGS_DISABLE_AVX2`を定義するとスカラー経路を強制して同じテストを実行できる。

`CaptionPgsRenderTests`ではアルファ復元の全65536組を従来の整数除算と比較し、行の前後へ書き込まないことも確認する。

## ARIB字幕描画のスモークテスト

`AribCaptionSmokeTests`は本体ライブラリの`CaptionPgsCheckRenderer`を呼び、静的リンクしたlibaribcaptionの`Context`と`Renderer`を初期化する。1920×1080キャンバスに透明背景で「日」を描画し、非透明画素があることを確認する。LinuxではfontconfigとFreeTypeを明示し、`Noto Sans CJK JP`の実ファイルと日本語グリフを解決する。指定フォントが欠落した場合の代替フォントへの暗黙の置換は失敗として扱う。Windowsではlibaribcaptionの標準バックエンドと既定の日本語フォントを使う。

Linuxでは`fonts-noto-cjk`と`fontconfig-config`を導入してから`meson test -C build-native AribCaptionSmokeTests --print-errorlogs`で実行する。Windowsでは`build_windows.cmd`が専用プロジェクトをビルドし、本体DLLと同じディレクトリで実行する。Windowsの本体DLLは`/execution-charset:shift_jis`でビルドされ診断文字列もShift-JISになるため、照合用リテラルが一致するようテストも同じ実行文字セットでビルドする。テスト自身はlibaribcaptionを直接リンクせず、本体ライブラリの描画経路を検証する。不正キャンバス寸法、空のフォント指定、Linuxでの欠落フォント、診断バッファの終端とnullバッファも確認する。

## 既知の問題 (追記)

- `MakeVFRBitrateZones` のゾーン数の上限判定は末尾のブロックを数えないため、細かく変化する入力では上限1000ではなく最大1001ゾーンになる。修正すると、通常の入力でも目標ゾーン数付近で併合が1回ずれ、ゾーン数が変わるため、実エンコードでの確認なしには修正しない。`bitrate_zones_properties` は現状の1001を上限として固定している。
