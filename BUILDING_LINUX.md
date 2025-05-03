# Linux向けAmatsukazeCLIビルド手順

このドキュメントでは、AmatsukazeCLIをLinux環境でビルドするための詳細な手順を説明します。

## 概要

AmatsukazeCLIはもともとWindows向けのMPEG2-TS変換ツールですが、このビルド手順ではmesonビルドシステムを使用してLinux環境でビルドできるようにしています。

主な依存関係：
- common（共通ユーティリティライブラリ）
- Caption（字幕処理ライブラリ）
- FFmpeg（メディア処理ライブラリ - システムから提供）
- OpenSSL（暗号化ライブラリ - MD5ハッシュ計算に使用）

## システム要件

- Linux OS（Ubuntu 20.04以上推奨）
- C++17対応コンパイラ
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
    libssl-dev libz-dev

# .NET
wget https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i ./packages-microsoft-prod.deb
sudo apt update
sudo apt install -y dotnet-sdk-8.0
```

### Fedora / RHEL / CentOS系

```bash
sudo dnf install -y gcc gcc-c++ git meson ninja-build pkg-config \
    ffmpeg-devel openssl-devel libz-devel

# .NET
sudo dnf install -y dotnet-sdk-8.0
```

### AviSynthのインストール

Linuxでは、AviSynth+をインストールする必要があります。

```bash
git clone https://github.com/AviSynth/AviSynthPlus.git
cd AviSynthPlus
mkdir build && cd build
cmake ..
make
sudo make install
```

### x264, x265, svt-av1

```bash
sudo apt install x264 x265 svt-av1
```

### mp4box

```bash
git clone https://github.com/gpac/gpac.git && cd gpac
./configure --static-bin
make -j8
sudo make install
```

### mkvmerge

```bash
sudo apt install mkvtoolnix
```

### chapter_exe

```bash
git clone https://github.com/tobitti0/chapter_exe
cd chapter_exe/src
sudo install -D -t /usr/local/bin chapter_exe
```

### join_logo_scp

```bash
git clone https://github.com/tobitti0/join_logo_scp
cd join_logo_scp/src
sudo install -D -t /usr/local/bin join_logo_scp
```

### L-SMASH

```bash
git clone https://github.com/l-smash/l-smash.git
cd l-smash/
./configure
make
sudo make install
```

## Amatsukazeのビルド

```bash
git clone https://github.com/rigaya/Amatsukaze.git --recursive
cd Amatsukaze
mkdir -p build && cd build
meson setup .. --prefix=$HOME/Amatsukaze
ninja install
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

## 開発情報

このLinux版ビルドは開発中です。問題や提案がある場合は、GitHubのIssueトラッカーで報告してください。

プロジェクトの改善にご協力いただける場合は、プルリクエストを歓迎します。
