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
- `--auto-logo-detect-threads <数値>`: スレッド数 `[0=自動=min(論理コア数, 16)]`

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

- service id は TS の program/service 情報から自動解決する
- service 名と日付が取得できた場合、ロゴ名は `サービス名(yyyy-MM-dd)` になる
- 情報が取得できない場合、ロゴ名は `情報なし`
