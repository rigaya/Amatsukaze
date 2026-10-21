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

`logo_background_retry` は、背景推定の救済探索を含む32画素AVX2処理をスカラー処理と比較します。半径1～32、上下左右の境界、乱数や単色などの入力、閾値境界について、有効画素マスクと背景値のビット一致を確認します。救済で最初に一致する辺の選択順、複数辺の平均、画像外の救済辺を飛ばす条件も固定値で検証します。AVX2非対応環境ではこのケースを省略します。

## 旧テストからの移行対応表

| 旧 GoogleTest ケース | 移行先 | 扱い |
|---|---|---|
| `CaptionText.UnicodeCharacterFormatting` | `caption_text_length` | 公開単体テストへ移行 |
| `VfrZonesBug` の通常ゾーン計算 | `bitrate_zones`, `bitrate_zones_properties` | 公開単体テストへ移行。通常ゾーン計算は固定値を比較する `bitrate_zones`。再現データ (`zone_param.dat`) は手元になく復元できないため、固定seedの乱数入力で不変条件を見る `bitrate_zones_properties` で代替する (映像範囲外にはみ出すCMゾーンの範囲外参照を見つけて修正) |
| VFR入力判定の実装内ケース | `vfr_input_detection` | 公開単体テストへ移行 |
| `CRC.*` | `crc32` | 公開単体テストへ移行。CRC-32/MPEG-2の検査値とPSIセクション検査で確認 |
| `Util.readOpt`, `Util.AutoBufferTest` | `bit_reader`, `auto_buffer` | 公開単体テストへ移行。旧テストが呼ぶ `--mode test_read_bits` は値を表示するだけで合否を判定しないため、BitReader/BitWriter/AutoBuffer を直接検証 |
| `MPEG2Parser`, `Pulldown`, `LargeTsParse` | 廃止 | 旧ケースは `ReadTS` (TSの分割) が例外なく終わることしか見ていない。同じ分割を通し、出力の復号・ストリーム構成・長さ・想定外のwarn/errorを判定する cli_itest の `basic_mkv_caption` (MPEG-2)、`input_rff`・`input_saekano_rff` (RFF/プルダウン)、`input_resolution_long`・`cm_logo_cut_full` (長時間素材)、公開 `mpeg2_video_parser`・`ts_selector` が同等以上に担保している |
| `H264Parser` | 廃止 | 旧ケースは `ReadTS` が例外なく終わることしか見ていない。H.264の放送を処理して出力を復号し判定する cli_itest の `input_resolution_1`・`input_resolution_3`・`input_resolution_4`、公開 `h264_video_parser`・`h264_pan_scan` が同等以上に担保している。旧素材 (`HD2-h264.ts`) は音声がAC3のみで、音声なしTSは未対応のため入力解析まで進まず、代替素材にならない |
| `H264Parser1Seg` | cli_itest `input_oneseg_only` | 非公開 (cli_itest) へ移管。ワンセグ混在TSからワンセグ (SID 25016、H.264 320x180) を `-s` で選んで処理し、サービス選択のログ・出力の寸法とコーデック・終了コードを判定する |
| `MPEG2PSVerifier` | `ps_writer_round_trip` | 公開単体テストへ移行。外部のMPEG-PS素材ではなく、本体の `PsStreamWriter` が書く出力を `PsStreamVerifier` と独立のPS解析で検証する |
| `AacDecodeVerifyTest` | `audio_aac_decoder_agreement` | 公開単体テストへ移行。旧ケースは外部のAACと参照WAVを使っていた。既知のPCMをテスト内でAACにエンコードし、同梱FAADとlibavcodecの復号結果を、AAC復号器の適合基準に基づく許容誤差で比較する |
| `WaveWriter` | `wave_header` | 公開単体テストへ移行。旧ケースはダミーWAVを書くだけだった。ヘッダ全フィールドを検証し、ブロックアライン等の不具合を修正 |
| `SplitDualMonoAAC` | `dual_mono_splitter` | 公開単体テストへ移行。旧ケースは分離を実行するだけだった。デュアルモノのADTSをテスト内で組み立て、分離結果をバイト単位で検証 |
| `AACDecodeTest` | `audio_silent_adts_decode` | 公開単体テストへ移行。旧ケースは実素材 (`a0-0-1.aac`) の復号エラーを見ていた。無音ADTSをFAADとlibavcodecで復号して全サンプル0を判定する。実放送の復号は cli_itest の `audio_encode_fdkaac`、`audio_mono_to_51_*` が通す |
| `encodeMpeg2Test`, `fileStreamInfoTest`, `LosslessTest`, `CaptionASSTest` | 廃止 | `encodeMpeg2Test` は通常エンコードの終了コードだけ、`fileStreamInfoTest`・`CaptionASSTest` は結果を書き出すだけで判定がなく、`LosslessTest` は呼び出す `test_lossless` モードが存在せず失敗する。cli_itest の `basic_mkv_caption`・`enc_x265`・`format_mp4` (エンコードと出力の判定)、`misc_resume_dir`・`misc_dump` (streaminfoの再利用)、`subtitle_mkv_enabled`・`subtitle_mp4_enabled`、`tests/amatsukaze` の字幕検証器 (ASS/SRTの内容と時刻) が同等以上に担保している |
| `DamemojiTest` | cli_itest `path_dame_moji` | 非公開 (cli_itest) へ移管。ケースのフォルダ名 (一時フォルダと出力先) にダメ文字を含め、エンコードからmuxまでの成功とログのパスを判定する |
| `LogoFrameTest` | cli_itest `cm_two_logos_best_first`, `cm_two_logos_best_second` | 非公開 (cli_itest) へ移管。旧ケースは最適ロゴを表示するだけだった。本体が選んだロゴをログに出すようにし、2つのロゴの指定順を入れ替えても一致するロゴが選ばれることを判定する |
| `Process.SimpleProcessTest` | 廃止 | `x264.exe --help` を起動して出力を表示するだけで判定がない。下位の `rgy_pipe` 層は `tools/ProcessLayerTest.cpp` が終了コード・シグナルを判定し、プロセスの起動失敗は cli_itest の `error_encoder_not_found` が判定する |
| `EncoderOptionTest01`～`EncoderOptionTest09` | `encoder_option` | 公開単体テストへ移行。旧テストの `--mode test_eo` は解析結果を表示するだけで合否を判定しないため、テスト専用の C ABI ラッパー `ParseEncoderOptionForTest` を通して期待値と比較する |
| `AudioTrack.SeparatePlanner` | `audio_track_separate_planner` | 公開単体テストへ移行。`PlanSeparateAudioTracks`/`ValidateAudioTrackPlans` はSTLコンテナを受け渡すため、テスト専用のC ABIラッパー `PlanAudioTracksForTest`/`ValidateAudioTrackPlansForTest` を通して、旧テストの期待値と比較する |
| `AudioTrack.MergePlanner` | `audio_track_merge_planner` | 公開単体テストへ移行。分離と同じC ABIラッパーで `PlanMergeAudioTracks` を検証する |
| `AudioTrack.SilentAdtsBytes` | `audio_silent_adts_bytes` | 公開単体テストへ移行。設計書の手組みバイト列との一致と拒否を `GenerateSilentAdtsFrameForTest` 経由で検証する。旧テストの `GetAudioSamplingFrequencyIndex` の逆引きは、分離プランのサンプリング周波数インデックスと未対応レートの拒否で確認する。PES経由の並行処理は既存の `audio_pes_pipeline` が担当 |
| `AudioTrackConvertTest.cpp` | `audio_channel_conversion`, `audio_convert_position`, `audio_convert_dual_mono`, `audio_convert_long_gap` | 公開単体テストへ移行。`ConvertAudioChannels` の係数と並びは `audio_channel_conversion` (テスト専用C ABI `ConvertAudioChannelsForTest`。旧テストはモノラル⇔5.1chの変換を拒否する前提だが、現行実装はステレオ経由で変換するため、現行仕様に合わせた)。CONVERT出力のフレーム数・COPY保持・サンプル位置・追加チャンネルの無音は `audio_convert_position`、cfg0デュアルモノは `audio_convert_dual_mono`、100秒前の履歴が無音CONVERTへ漏れないことは `audio_convert_long_gap` が判定する。`run_audio_track_seam_test.py` の `--experiment`・`--oracle` は設計判断用の実験で廃止 |
| `AudioTrackStreamReformTest.cpp`, `AudioTrackFaadSilenceTest.cpp` | `audio_reform_separate`, `audio_reform_merge`, `audio_reform_unsupported_fallback`, `audio_reform_cfg0_dual_mono`, `audio_builder_*`, `audio_silent_adts_decode` | 公開単体テストへ移行。素材を使わず、StreamReformの音声プラン作成と `BuildAudioTrack`、無音ADTSの復号を判定する。詳細は下記の「StreamReformと音声トラック構築の合成入力テスト」 |
| `run_audio_track_test.py` | 廃止 | `compile_commands.json` から旧テストを単体ビルドして実行するだけのスクリプト。個別のテストを公開側へ移したため不要 |
| `CLI.ArgumentTest` | `command_line_arguments` | 公開単体テストへ移行。旧テストの `--mode test_parseargs` は例外が出ないことしか見ていないため、`AmatsukazeCLI.hpp` の static `parseArgs` の結果を、同じ翻訳単位で定義したテスト専用C ABI `ParseCommandLineForTest` が「キー=値」の改行区切りテキストへ直列化して返し、呼出側バッファ経由で値を比較する。旧テストの3パターン (10進と`0x6308`のサービスID、未知オプション`--ourput`の拒否) に加え、全オプション指定時の入出力パス・エンコーダ種別・オプション・muxer・timelineeditor・JSON・dumpの値、パス区切りの`/`への正規化、既知拡張子 (`.mp4`/`.mkv`/`.m2ts`/`.ts`) だけが除かれる出力パス、最小構成の既定値、モード別の必須引数、値が欠けた末尾オプション、整数オプションの厳密な解析 (空、`12abc`、`1.5`、intの範囲外、不正な16進を引数の拒否として扱い、サービスIDは`0x`と`0X`の両方を16進として受理する)、種別を表す値 (エンコーダ、音声エンコーダ、デコーダ) の不明な名前の拒否とサーバーが渡す綴りの受理、範囲外の値、実況マスクの有効化条件を確認する。実行ファイル位置やPATH探索に依存する既定の実行ファイル名は比較せず、比較する実行ファイルは存在しない絶対パスで明示指定する。作業ディレクトリは取得に一時ディレクトリの作成を要するため比較しない |
| `DecodePerformance` | 廃止 | libavcodecの復号速度を表示するだけで判定がなく、本体のロジックを通らない。性能の確認は cli_itest の所要時間で見る |

旧 `AmatsukazeUnitTest/` と GoogleTest (`googletest` サブモジュール、`include_win/gtest/`) は、上表の移行を終えて撤去しました。C# の `AmatsukazeServerTest` xUnit は維持し、ネットワークを使う任意テストは `Category=Network` としてオフラインの標準単体テストから分離します。標準群は次で実行します。

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

## AAC復号の一致とCONVERTの位置

素材を使わず、既知のPCMから生成したAACを使う。PCMは固定seedの周波数・位相を持つ正弦波の和 (CONVERTの入力にはさらに雑音を加える) で、本体の変換用エンコーダ (libavcodecのaac) でADTSにする。テスト専用C ABIは `EncodePcmToAdtsForTest`、`DecodeAdtsToPcmForTest` (同梱FAADまたはlibavcodecで復号してインターリーブのfloatで返す)、`ConvertAdtsTrackForTest` (出力位置ごとの参照とCOPY/CONVERTを指定して `BuildAudioTrack` で書き出す) である。無音ADTSの復号 (`audio_silent_adts_decode`) も同じ復号関数を通る。

- `audio_aac_decoder_agreement` (旧 `AacDecodeVerifyTest`): モノラルとステレオの64フレームを、FAADとlibavcodecで復号して比較する。libavcodecは全フレーム、FAADは先頭1フレーム分の出力を出さない (モノラルはPS対応で2chへ展開される) ので、フレーム数と、FAADの出力がlibavcodecの1フレーム後ろと対応することを確認する。許容誤差は、AAC復号器の適合基準 (ISO/IEC 14496-4) の考え方による。16bit出力のフルアキュラシーでは、参照復号との差のRMSが16bit量子化誤差のRMS (2^-15/√12 ≒ 8.8e-6) 以下、最大誤差が2^-14 (≒ 6.1e-5) 以下であることが求められるので、この値をそのまま使う。モノラルの実測は差のRMSが7e-7、最大誤差が7e-6で、この基準に十分収まる (ステレオも基準内)。復号結果の最大振幅が0.2以上であることも確認し、無音どうしの一致で通らないようにしている。
- `audio_convert_position` (旧 `run_audio_track_convert_test.py` の本体): 2ch→5.1ch、5.1ch→2ch、モノラル→2ch、2ch→モノラルについて、CONVERT区間の長さ (1、2、96) と前後のCOPY (8フレームまたはなし) を変えた8通りで、出力のフレーム数が入力トラックと一致すること、前後のCOPYフレームがバイト単位で変化しないこと、復号後のサンプル数、CONVERT元を単独で復号して求めた変換後の期待信号との相互相関のピークが±32サンプルの探索でラグ0であること (相関0.9超)、5.1chへの変換で追加の4チャンネルが無音であることを確認する。ラグと相関は96フレームの場合に、両復号器の初期の重なりを除いた内部 (前後4フレームを除く) で測る。
- `audio_convert_dual_mono` (旧 `run_audio_track_convert_test.py` のデュアルモノ): cfg0のデュアルモノの無音12フレームを、主音声・副音声それぞれからステレオへCONVERTしても、フレーム数が保たれ完全な無音のままであることを確認する。
- `audio_convert_long_gap` (旧 `run_audio_track_seam_test.py --long-gap-source`): 有音のステレオ1フレームの後に、100秒 (90kHzで9000000) 後の無音のステレオ12フレームを置き、無音側を5.1chへCONVERTすると、出力が完全な無音であること (有音の履歴が漏れないこと) を確認する。対照として、時刻が連続している場合は有音のフレームが履歴として使われて出力が無音でなくなることも確認し、この判定が漏れを検出できる条件であることを保っている。

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

## ロゴ統計収集の分割テスト

`logo_collect_partition` は単フレーム処理とバッチ処理で共用するX方向の分割境界を検証する。幅・余白・半径・分割数・32画素整列の有無を組み合わせ、外端保持、単調性、全画素をちょうど一度だけ覆うことを確認する。整列無効時は従来の等分境界と一致し、整列によって空区間になる狭い分割は元の境界を保持する。実例の幅288・余白16・半径32・2分割で内部境界128になること、空ROIと大きい幅で安全に分割できることも確認する。

選択実行は `AmatsukazeNativeTests --select logo_collect_partition`。CPUのAVX2対応には依存しない。

## ロゴ残差重みの画素固定パラメータ

`logo_residual_weight_params` は、再重み付けで画素ごとに事前計算する有効判定 (`trust > 1e-4`) と許容幅 `sigma` を、共有ヘッダから独立に記述した従来式とbit単位で比較する。閾値の直前・一致・直後、sigmaの下限、正負のゼロ、非正規化数、最大有限値、NaN、正負の無限大、および固定seedの乱数入力を確認する。無効な画素のsigmaは使用しない既定値1とし、有効な画素のsigmaは従来の除算にそのまま渡す。逆数化や指数関数の近似は行わない。

選択実行は `AmatsukazeNativeTests --select logo_residual_weight_params`。
