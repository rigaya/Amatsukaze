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
    libfaad-dev libssl-dev libz-dev
```

### Fedora / RHEL / CentOS系

```bash
sudo dnf install -y gcc gcc-c++ git meson ninja-build pkg-config \
    ffmpeg-devel faad2-devel openssl-devel libz-devel
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

## トラブルシューティング

1. **ビルドエラー**:
   - 最新の依存パッケージがインストールされていることを確認してください
   - `meson configure` でビルド設定を確認・修正できます
   - OpenSSLのエラーが出る場合は、libssl-devがインストールされていることを確認してください

2. **実行時エラー**:
   - 必要なライブラリが適切にインストールされていることを確認してください
   - 権限に関する問題が発生した場合は、適切な権限で実行してください

3. **C#プロジェクトのビルドエラー**:
   - .NET環境の場合、.NET SDKがインストールされていることを確認してください
   - Mono環境の場合、monoがインストールされていることを確認してください
   - ビルド時に「Unknown method "name" in object」エラーが発生する場合は、mesonのバージョンによる問題です。`meson setup`コマンド時に`--reconfigure`オプションを追加して再設定を行ってください：
     ```bash
     meson setup .. --prefix=$HOME/Amatsukaze -Dbuild_csharp_projects=true --reconfigure
     ```
   - 「No such file or directory: AmatsukazeServer.dll」などのエラーが発生する場合は、ビルド順序の問題である可能性があります。まず`AmatsukazeServer`を個別にビルドしてから他のプロジェクトをビルドしてください：
     ```bash
     cd AmatsukazeServer && dotnet build
     cd ..
     ```
   - .NETバージョンの問題が発生する場合は、各プロジェクトのmeson.buildファイル内のnet6.0を、インストール済みの.NETバージョン（net7.0や8.0など）に変更するか、.NET 6 SDKをインストールしてください。
   - Mono関連のエラーが発生する場合は、Monoの完全なインストールを確認してください：
     ```bash
     sudo apt install mono-complete
     ```

4. **既知の問題**:
   - Linuxのファイルパス規則とWindowsの違いに注意（バックスラッシュと設定ファイルパス）
   - 一部のシステム依存コードはプラットフォーム判定で処理されますが、テストされていない機能もあります

## 開発情報

このLinux版ビルドは開発中です。問題や提案がある場合は、GitHubのIssueトラッカーで報告してください。

プロジェクトの改善にご協力いただける場合は、プルリクエストを歓迎します。

## C#プロジェクトのビルド（Linuxでの対応版）

Amatsukaze環境では、以下のC#プロジェクトをLinux上でビルドすることができます：

- AmatsukazeServer：サーバーコアライブラリ
- AmatsukazeServerCLI：サーバーのコマンドラインインターフェース
- AmatsukazeAddTask：タスク追加ユーティリティ
- AmatsukazeGUI：GUI（オプション機能）

### 前提条件

C#プロジェクトをビルドするには、以下のいずれかの環境が必要です：

#### .NET Core/.NET 6以降

```bash
# Ubuntu/Debian系
wget https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i ./packages-microsoft-prod.deb
sudo apt update
sudo apt install -y dotnet-sdk-8.0

# Fedora系
sudo dnf install dotnet-sdk-8.0
```

#### Mono (代替手段）

```bash
# Ubuntu/Debian系
sudo apt install mono-complete

# Fedora系
sudo dnf install mono-complete
```

### ビルド方法

C#プロジェクトをビルドするには、`build_csharp_projects`オプションを有効にします：

```bash
mkdir -p build && cd build
meson setup .. --prefix=$HOME/Amatsukaze -Dbuild_csharp_projects=true
ninja
ninja install
```

GUIをビルドする場合は、`build_gui`オプションも有効にします：

```bash
meson setup .. --prefix=$HOME/Amatsukaze -Dbuild_csharp_projects=true -Dbuild_gui=true
```

### 注意事項

1. **GUIのLinux対応について**:
   - AmatsukazeGUIはWindows向けのWPFアプリケーションですが、.NET 6以降では一部Linuxでも動作します
   - Monoを使用する場合、WPFのサポートは限定的であり、完全な機能は期待できません
   - Linuxでのサーバー利用ではCLIベースの利用を推奨します

2. **Windows依存コードの互換性**:
   - 一部のWindowsネイティブAPIに依存するコードは、Linux環境では代替実装または制限付きで動作します
   - 特にファイルシステム関連の機能やWin32 APIを使用する部分は注意が必要です

3. **実行方法**:
   - .NET環境の場合: `dotnet AmatsukazeServerCLI.dll`
   - Mono環境の場合: `mono AmatsukazeServerCLI.exe`
   - または直接実行可能な場合: `./AmatsukazeServerCLI`

4. **既知の問題**:
   - Linuxのファイルパス規則とWindowsの違いに注意（バックスラッシュと設定ファイルパス）
   - 一部のシステム依存コードはプラットフォーム判定で処理されますが、テストされていない機能もあります

## トラブルシューティング

1. **ビルドエラー**:
   - 最新の依存パッケージがインストールされていることを確認してください
   - `meson configure` でビルド設定を確認・修正できます
   - OpenSSLのエラーが出る場合は、libssl-devがインストールされていることを確認してください

2. **実行時エラー**:
   - 必要なライブラリが適切にインストールされていることを確認してください
   - 権限に関する問題が発生した場合は、適切な権限で実行してください

3. **C#プロジェクトのビルドエラー**:
   - .NET環境の場合、.NET SDKがインストールされていることを確認してください
   - Mono環境の場合、monoがインストールされていることを確認してください

4. **Mono関連のエラーが発生する場合**:
   - Monoの完全なインストールを確認してください：
   ```bash
   sudo apt install mono-complete
   ```
   - 特にWPF関連のMonoパッケージが必要です。 