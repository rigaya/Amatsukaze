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
| `CRC.*`, `Util.readOpt`, `Util.AutoBufferTest` | 保留 | 呼出先と期待値を再確認してから追加 |
| `MPEG2Parser`, `H264Parser`, `H264Parser1Seg`, `Pulldown`, `LargeTsParse`, `MPEG2PSVerifier` | 非公開コンポーネントテスト | 外部の映像・TS・MPEG-PS素材に依存 |
| `AacDecodeVerifyTest`, `WaveWriter`, `SplitDualMonoAAC`, `AACDecodeTest` | 非公開コンポーネントテスト | 外部音声素材または出力検証が必要 |
| `encodeMpeg2Test`, `fileStreamInfoTest`, `DamemojiTest`, `LosslessTest`, `LogoFrameTest`, `CaptionASSTest` | 非公開コンポーネント/統合テスト | 外部素材、AviSynth、ロゴまたは出力ファイルに依存 |
| `Process.SimpleProcessTest` | 非公開コンポーネントテスト | 子プロセス実行を伴う |
| `EncoderOptionTest01`～`EncoderOptionTest09`, `CLI.ArgumentTest` | 保留 | 入力と失敗期待を明文化してから追加 |
| `DecodePerformance` | 非公開ベンチマーク | 性能計測であり単体テストの合否に含めない |

既存の `AmatsukazeUnitTest/` と GoogleTest は、この移行段階では削除しません。C# の `AmatsukazeServerTest` xUnit は維持し、ネットワークを使う任意テストは `Category=Network` としてオフラインの標準単体テストから分離します。標準群は次で実行します。

```bash
dotnet test AmatsukazeServerTest/AmatsukazeServerTest.csproj --filter "Category!=Network"
```

`bitrate_zones` の入力はフレーム数+1個の終端時刻を含める。8フレーム単位で計算した期待値は `0-40: 2.5`、`40-64: 1.35`、`64-128: 1.1375`、`128-150: 2.0` である。第2・第3ゾーンの値はそれぞれ `(1.5+1.5+1.05)/3` と `(0.6+0.6+1+1+1.5+2+1.2+1.2)/8` から独立に計算できる。19単位のコスト上限は `19*0.15=2.85` で、5ゾーン時の累積値 `2.366666...` に最小併合コスト `0.8` を加えた4ゾーン時は `3.166666...` になる。実装は追加前の累積値で継続可否を判定するため、次の反復で停止し、4→3の併合コスト `0.927272...` は適用されない。旧テストの `40-128: 約1.195` はこの未実施の併合を前提とするため移植しない。

Windows では本体とテストを同じ Visual C++ ツールセットおよび `/MT` ランタイムでビルドする。`bitrate_zones` と `vfr_input_detection` はテスト専用の C ABI ラッパーを通し、DLL境界で `std::vector` の所有権や実装を受け渡ししない。
