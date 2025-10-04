# Linux向けAmatsukazeServerのビルド手順

## 必要なツールと依存パッケージのインストール

```bash
sudo apt update
sudo apt install -y build-essential git wget curl nasm cmake meson ninja-build pkg-config autoconf automake libtool \
    libssl-dev libz-dev
```
次に .NET Core 9.0 SDKをインストールします。下記はUbuntu 24.04の例を示します。その他の環境については、[リンク先](https://learn.microsoft.com/ja-jp/dotnet/core/install/linux)を参照してください。

```bash
# .NET
wget https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i ./packages-microsoft-prod.deb
sudo apt update
sudo apt install -y dotnet-sdk-9.0
```

## AviSynthのインストール

Linuxでは、AviSynth+をインストールする必要があります。[こちら](https://github.com/rigaya/AviSynthCUDAFilters/releases)から最新版のdebパッケージをダウンロードしてインストールしてください。

```bash
sudo apt install -y ./avisynth_<version>_amd64_Ubuntuxx.xx.deb
```

自ビルドする場合は[こちら](https://github.com/rigaya/AviSynthCUDAFilters/blob/master/README_LINUX.md)を参考にしてください。

## Amatsukaze本体のビルド

下記では、Amatsukazeを ```$HOME/Amatsukaze``` にインストールする例を示します。

```./scripts/build.sh``` により下記が自動的に実行されます。

- AmatsuakzeCLI, libAmatsukaze.soのビルド (C++)
  - 依存するffmpeg関連ライブラリのビルドを含む
- AmatsuakzeServer, AmatsuakzeServerCLI, AmatsuakzeAddTask のビルド (C# dotnet)
- インストール先への実行ファイルの配置

```bash
git clone https://github.com/rigaya/Amatsukaze.git --recursive
cd Amatsukaze
./scripts/build.sh $HOME/Amatsukaze
```


## 各Avisynthプラグインへのリンクの作成
  
実際にAmatsuakzeを使用するには、各種Avisynthプラグインをインストール後、```exe_files/plugins64```にそのリンクを作成する必要があります。

```./scripts/install.sh```を実行するとインストール済みの各Avisynthプラグインへのリンクが```exe_files/plugins64```に自動的に作成されます。

```bash
cd $HOME/Amatsukaze
./scripts/install.sh
```