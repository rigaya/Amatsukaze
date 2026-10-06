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
| `CLI.ArgumentTest` | 保留 | 引数解析が `AmatsukazeCLI.hpp` 内の static 関数で、実行ファイル位置に依存する既定値を含むため、公開方法を設計してから追加 |
| `DecodePerformance` | 非公開ベンチマーク | 性能計測であり単体テストの合否に含めない |

既存の `AmatsukazeUnitTest/` と GoogleTest は、この移行段階では削除しません。C# の `AmatsukazeServerTest` xUnit は維持し、ネットワークを使う任意テストは `Category=Network` としてオフラインの標準単体テストから分離します。標準群は次で実行します。

```bash
dotnet test AmatsukazeServerTest/AmatsukazeServerTest.csproj --filter "Category!=Network"
```

`bitrate_zones` の入力はフレーム数+1個の終端時刻を含める。8フレーム単位で計算した期待値は `0-40: 2.5`、`40-64: 1.35`、`64-128: 1.1375`、`128-150: 2.0` である。第2・第3ゾーンの値はそれぞれ `(1.5+1.5+1.05)/3` と `(0.6+0.6+1+1+1.5+2+1.2+1.2)/8` から独立に計算できる。19単位のコスト上限は `19*0.15=2.85` で、5ゾーン時の累積値 `2.366666...` に最小併合コスト `0.8` を加えた4ゾーン時は `3.166666...` になる。実装は追加前の累積値で継続可否を判定するため、次の反復で停止し、4→3の併合コスト `0.927272...` は適用されない。旧テストの `40-128: 約1.195` はこの未実施の併合を前提とするため移植しない。

## 既知の問題

- H.264パーサはSEIのbuffering_periodで得たDTSを次のアクセスユニットへ引き継いでいないため、PESにPTS/DTSがないフレームはSEIのpic_timingから時刻を補完できず-1のままになる。PESのPTSとSEIから求めた値の照合も実質的に行われない。修正すると照合が有効になり、実放送で警告と`AMT_ERR_H264_PTS_MISMATCH`が増える可能性があるため、実TSでの確認なしには修正しない。放送のH.264ではPESに常にPTSが付くため実害は小さい。

テストは TCHAR を原則とし、出力は `_ftprintf`、書式化は `strsprintf` を使う。`common` の静的ライブラリをリンクし、本体DLLでは非公開の `StringFormat`（`THROW` が使用）のため `Amatsukaze/StringUtils.cpp` もテストに組み込む。

Windows では本体とテストを同じ Visual C++ ツールセットおよび `/MT` ランタイムでビルドする。`bitrate_zones` と `vfr_input_detection` はテスト専用の C ABI ラッパーを通し、DLL境界で `std::vector` の所有権や実装を受け渡ししない。

`caption_pgs_canvas_size` は、出力映像のフレームサイズとSARからPGSのキャンバス (表示サイズ) を求める `CaptionPgsCanvasSize` を、テスト専用の C ABI `CaptionPgsCanvasSizeForTest` を通して検証する。HD (1440x1080 SAR 4:3 → 1920x1080)、SD 16:9 (720x480 SAR 32:27 → 853x480)、SARが1未満のSD 4:3 (高さを伸ばして720x540)、SAR不明、`--sar` の優先と片方だけの指定の無視、長辺4096を超える場合の比例縮小 (横長・縦長・8K) と、不正な引数を確認する。

## PGSエンコーダ単体テスト

`PgsEncoderTests`はPGS実装を直接コンパイルし、FFmpegの`pgssub`デコーダでSUPを往復検証する。本体DLLやlibaribcaptionに依存しない。Linuxでは`meson compile -C build-native PgsEncoderTests`と`meson test -C build-native PgsEncoderTests --print-errorlogs`で実行する。Windowsでは`build_windows.cmd`が専用`PgsEncoderTests.vcxproj`をビルドして実行する。

255色以下ではRGBAの各成分の誤差を±2以内とし、透明画素のRGBは評価対象外とする。4096色のRGBAグラデーションでは非透明画素のRGBA成分全体のPSNRを30dB以上とする。480/576/577/1080行で色変換の切替境界を検証し、領域結合、透明行列の削除、ODS分割、RLEの長さ境界と行末、PCS/WDS/ENDの固定バイト列、消去WDSと直前エポックのウィンドウ定義の一致、消去後の透明画像、隣接イベントの消去省略、255/256色の共有パレット境界、半透明のsource-over合成、不正入力、空・全透明イベント、PTSとcomposition番号の折り返し、ファイル出力とメモリ出力の一致も検証する。

## ARIB字幕描画のスモークテスト

`AribCaptionSmokeTests`は本体ライブラリの`CaptionPgsCheckRenderer`を呼び、静的リンクしたlibaribcaptionの`Context`と`Renderer`を初期化する。1920×1080キャンバスに透明背景で「日」を描画し、非透明画素があることを確認する。LinuxではfontconfigとFreeTypeを明示し、`Noto Sans CJK JP`の実ファイルと日本語グリフを解決する。指定フォントが欠落した場合の代替フォントへの暗黙の置換は失敗として扱う。Windowsではlibaribcaptionの標準バックエンドと既定の日本語フォントを使う。

Linuxでは`fonts-noto-cjk`と`fontconfig-config`を導入してから`meson test -C build-native AribCaptionSmokeTests --print-errorlogs`で実行する。Windowsでは`build_windows.cmd`が専用プロジェクトをビルドし、本体DLLと同じディレクトリで実行する。テスト自身はlibaribcaptionを直接リンクせず、本体ライブラリの描画経路を検証する。不正キャンバス寸法、空のフォント指定、Linuxでの欠落フォント、診断バッファの終端とnullバッファも確認する。
