# AmatsukazeGenLogo

`AmatsukazeGenLogo` は TS 入力から局ロゴ向けの自動ロゴ枠検出とロゴ生成を CLI でまとめて行う実行ファイルです。
対象は日本のテレビ放送で右上に表示される放送局ロゴです。

## 概要

- `Amatsukaze.dll` / `libAmatsukaze.so` をロードして実行する
- `--logo-range` 未指定時は自動ロゴ枠検出を行う
- 自動ロゴ枠検出成功後、その矩形でロゴ生成を行う
- どこかで失敗した場合は最終出力を保存しない

## 使い方

```bash
AmatsukazeGenLogo -i input.ts -o output.lgd
```

手動で矩形を指定する場合:

```bash
AmatsukazeGenLogo -i input.ts -o output.lgd --logo-range 1230,64,156,38
```

AviUtl 向け lgd を出力する場合:

```bash
AmatsukazeGenLogo -i input.ts -o output.lgd --aviutl-lgd
```

デバッグ画像を保存する場合:

```bash
AmatsukazeGenLogo -i input.ts -o output.lgd --debug-dir ./debug_logo
```

## オプション

### メイン

- `-i`, `--input <path>`: 入力 TS ファイル
- `-o`, `--output <path>`: 出力 lgd ファイル

### ロゴ枠

- `--logo-range <x>,<y>,<w>,<h>`: ロゴ枠を手動指定
- `--auto-logo-detect-search-frames <数値>`: 検索フレーム数 `[10000]`
- `--auto-logo-detect-div-x <数値>`: 分割数 X `[5]`
- `--auto-logo-detect-div-y <数値>`: 分割数 Y `[5]`
- `--auto-logo-detect-block-size <数値>`: ブロックサイズ `[32]`
- `--auto-logo-detect-threshold <数値>`: 閾値 `[12]`
- `--auto-logo-detect-margin-x <数値>`: マージン X `[6]`
- `--auto-logo-detect-margin-y <数値>`: マージン Y `[6]`
- `--auto-logo-detect-threads <数値>`: スレッド数 `[0=自動]`

### ロゴ生成

- `--logo-gen-threshold <数値>`: ロゴ生成の背景閾値
- `--logo-gen-samples <数値>`: ロゴ生成の最大サンプル数

未指定時は次の値を使います。

- `logo-gen-threshold`: `auto-logo-detect-threshold` の値
- `logo-gen-samples`: `auto-logo-detect-search-frames` の値

### その他

- `--aviutl-lgd`: AviUtl 向けベース形式 lgd を保存
- `--debug-dir <path>`: 自動ロゴ枠検出のデバッグ画像を保存
- `--help`: ヘルプ表示
- `--version`: バージョン表示

## 出力について

- 通常出力は Amatsukaze 拡張 lgd
- `--aviutl-lgd` 指定時は AviUtl 向け base-only lgd
- 出力ファイルは一時ファイルに保存後、成功時のみ最終パスへ配置する
- 出力先に既存ファイルがある場合は上書きせず、`<ファイル名>-yyyyMMdd_HHmmss.lgd` 形式の別名で保存する

## 備考

- 動画デコーダのスレッド数は論理コア数から自動設定し、1～8に制限する
- MPEG-2では環境変数 `AMT_LOGO_GOP_DECODERS=4` でGOP並列復号を利用できる。既定値の `1` は従来経路。初期走査と初期ロゴ生成の両方に適用する。
- `AMT_LOGO_GOP_DECODERS=0` ではCM解析と共通の方針で自動配分する。CPU数の割り算の余りと内部4スレッド超過を避け、同点なら8デコーダーに近い候補を選ぶ（最大15）。CPU数は既存処理と同じ論理CPU数を基準とし、内部スレッド数はCPU数÷デコーダー数、最大8。4 CPUなら4×1、16 CPUなら8×2、32 CPUなら8×4になる。
- GOP並列復号は既定で各デコーダーに全体のスレッド数を分配し、1ジョブを最大8 GOP、各デコーダーの出力待ちバッファを100フレームまでとして構成する。`AMT_LOGO_GOP_THREADS`、`AMT_LOGO_GOPS_PER_JOB`、`AMT_LOGO_GOP_BUFFER_FRAMES` で比較・調整できる。設計と制約は `LogoGopDecoder.txt` を参照。
- `AMT_LOGO_GOP_BUFFER_FRAMES` が正の値なら、MiBによるジョブ短縮を行わず、GOP長とバッファ容量を独立に指定できる。`0` を指定した場合は `AMT_LOGO_GOP_BUFFER_MIB` の目安（既定2GiB）に合わせてGOP境界でジョブを短くし、担当区間分の出力容量を用意する。内部の参照画像や大きな1 GOPなどによって保持量の目安は超える場合がある。
- 背景推定と再走査時の統計の再利用は、自動ロゴ枠検出の並列数によらず行う（メモリ不足などでキャッシュを利用できない場合を除く）
- 8bit入力で背景キャッシュと非加重統計が揃っている場合、重みが常に1となる画素の再重み付け走査を省く。比較・調査時は環境変数 `AMT_LOGO_REWEIGHT_SKIP_FIXED=0` で従来の全画素走査に戻せる。
- service id は TS の program/service 情報から自動解決する
- service 名と日付が取得できた場合、ロゴ名は `サービス名(yyyy-MM-dd)` になる
- 情報が取得できない場合、ロゴ名は `情報なし`
