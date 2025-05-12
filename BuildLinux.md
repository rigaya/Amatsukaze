# Linux向けAmatsukazeServerのビルド手順

## 必要なツールと依存パッケージのインストール

```bash
sudo apt update
sudo apt install -y build-essential git wget curl nasm yasm cmake meson ninja-build pkg-config \
    autoconf automake libtool \
    libssl-dev libz-dev

# .NET
wget https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i ./packages-microsoft-prod.deb
sudo apt update
sudo apt install -y dotnet-sdk-8.0
```

## AviSynthのインストール

Linuxでは、AviSynth+をインストールする必要があります。

```bash
(curl -s https://api.github.com/repos/rigaya/AviSynthCUDAFilters/releases/latest \
  | grep "browser_download_url.*deb" | grep "avisynth_" | grep "Ubuntu24.04" | grep "amd64" | cut -d : -f 2,3 | tr -d \" \
  | wget -i - -O avisynth.deb \
  && sudo apt install -y ./avisynth.deb \
  && rm ./avisynth.deb)
```

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
