# Linux向けAmatsukazeCLIビルド手順

このドキュメントでは、AmatsukazeCLIをLinux環境でビルドするための詳細な手順を説明します。

## 概要

AmatsukazeCLIはもともとWindows向けのMPEG2-TS変換ツールですが、このビルド手順ではmesonビルドシステムを使用してLinux環境でビルドできるようにしています。

主な依存関係：
- common（共通ユーティリティライブラリ）
- libfaad2（AACデコーダー）
- Caption（字幕処理ライブラリ）
- FFmpeg（メディア処理ライブラリ - システムから提供）
- OpenSSL（暗号化ライブラリ - MD5ハッシュ計算に使用）

## システム要件

- Linux OS（Ubuntu 20.04以上推奨）
- C++14対応コンパイラ（GCC 7以上またはClang 6以上）
- meson & ninja ビルドシステム
- pkg-config
- FFmpegライブラリ（libavcodec, libavformat, libavutil, libswscale, libswresample）
- OpenSSLライブラリとヘッダーファイル

## 依存パッケージのインストール

### Ubuntu / Debian系

```bash
sudo apt update
sudo apt install -y build-essential git meson ninja-build pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    libfaad-dev libssl-dev
```

### Fedora / RHEL / CentOS系

```bash
sudo dnf install -y gcc gcc-c++ git meson ninja-build pkg-config \
    ffmpeg-devel faad2-devel openssl-devel
```

### Arch Linux系

```bash
sudo pacman -S --needed base-devel git meson ninja pkg-config \
    ffmpeg faad2 openssl
```

### AviSynthのインストール

Linuxでは、AviSynthの代替実装であるAviSynth+またはAviSynthNeoをインストールする必要があります。

#### Ubuntu / Debian系
```bash
# AviSynth+のインストール
sudo add-apt-repository ppa:djcj/vapoursynth
sudo apt-get update
sudo apt-get install avisynth+
```

#### 手動インストール
AviSynth+のソースコードからインストールする場合：
```bash
git clone https://github.com/AviSynth/AviSynthPlus.git
cd AviSynthPlus
mkdir build && cd build
cmake ..
make
sudo make install
```

## ビルド手順

1. リポジトリのクローン（すでにクローン済みの場合はスキップ）:

```bash
git clone https://github.com/your-username/Amatsukaze.git
cd Amatsukaze
```

2. ビルドディレクトリの作成:

```bash
mkdir -p build
cd build
```

3. Mesonプロジェクトの設定:

```bash
meson setup ..
```

4. ビルドの実行:

```bash
ninja
```

5. インストール（オプション）:

```bash
sudo ninja install
```

## 実行方法

ビルドしたAmatsukazeCLIは以下のように実行できます：

```bash
./AmatsukazeCLI/AmatsukazeCLI -i <input.ts> -o <output.mp4>
```

または、インストールした場合：

```bash
AmatsukazeCLI -i <input.ts> -o <output.mp4>
```

詳細なコマンドラインオプションを確認するには：

```bash
./AmatsukazeCLI/AmatsukazeCLI --help
```

## 注意事項

1. **既知の制限**:
   - 一部のWindowsに依存する機能は制限または無効化されています
   - ffmpegはシステムライブラリを使用します
   - GUI機能は利用できません

2. **デバッグ方法**:
   - デバッグビルドを行うには: `meson setup --buildtype=debug ..`
   - トレースログを有効にするには: `-v` オプションを追加

3. **ビルド設定のカスタマイズ**:
   - オプション機能の無効化: `meson setup -Doption=false ..`
   - ビルド設定の変更: `meson configure -Doption=value`

## トラブルシューティング

1. **ビルドエラー**:
   - 最新の依存パッケージがインストールされていることを確認してください
   - `meson configure` でビルド設定を確認・修正できます
   - OpenSSLのエラーが出る場合は、libssl-devがインストールされていることを確認してください

2. **実行時エラー**:
   - 必要なライブラリが適切にインストールされていることを確認してください
   - 権限に関する問題が発生した場合は、適切な権限で実行してください

## 開発情報

このLinux版ビルドは開発中です。問題や提案がある場合は、GitHubのIssueトラッカーで報告してください。

プロジェクトの改善にご協力いただける場合は、プルリクエストを歓迎します。 