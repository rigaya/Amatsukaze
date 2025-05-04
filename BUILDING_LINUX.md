# Linux向けAmatsukazeServer

**注意**
現在建設中でたぶん対応しきれていない箇所が多く、バグだらけです。

## 概要

AmatsukazeServerCLI と AmatsukazeCLI、AmatsukazeAddTask をLinux対応作業中です。

AmatsukazeGUIは.NETのWPFが使われており、WPFはLinuxに対応していないようなので、Linux対応は難しいです。

そのため、LinuxでAmatsukazeServerCLIを起動して、WindowsからAmatsukazeGUIで接続する形を想定しています。

また、タスクのキューへの追加はAmatsukazeAddTaskの利用を想定しています。

## 作業状況と作業予定

- Linux対応済み
  - AmatsukazeCLI (Windows: cp932, Linux: utf8)
    - AvisynthのフィルタはひとまずCPU版は動作確認
  - AmatsukazeServerCLI
  - AmatsukazeAddTask

- 今後Linux対応できるかも?
  - GPU版のAvisynthのフィルタを使用できる?
    - Avisynth+はCUDA対応しているらしいが、AvisynthNeoと互換性があるのかどうか…
  - 設定画面へのドラッグドロップ
  - 音声エンコード

- Linux対応困難
  - AmatsukazeGUI
  - エンコード中の一時停止

- その他対応予定なし
  - エンコード後、スリープ・シャットダウン
  - スレッドアフィニティの指定
  - ffmpegに対する独自拡張の取り込み
  - 他のエンコーダの追加等


## インストール手順


### 依存パッケージのインストール

#### Ubuntu / Debian系

```bash
sudo apt update
sudo apt install -y build-essential git cmake meson ninja-build pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    libssl-dev libz-dev

# .NET
wget https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i ./packages-microsoft-prod.deb
sudo apt update
sudo apt install -y dotnet-sdk-8.0
```

#### Fedora / RHEL / CentOS系

```bash
sudo dnf install -y gcc gcc-c++ git cmake meson ninja-build pkg-config \
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
make -j$(nproc)
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
make -j$(nproc)
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

### L-SMASH (muxer, timelineeditor)

```bash
git clone https://github.com/l-smash/l-smash.git
cd l-smash/
./configure
make -j$(nproc)
sudo make install
```

### fdkaac

```bash
git clone https://github.com/mstorsjo/fdk-aac.git
cd fdk-aac
./autogen.sh
./configure
make -j$(nproc)
sudo make install
cd ..

git clone https://github.com/nu774/fdkaac.git
cd fdkaac
autoreconf -i
./configure
make -j$(nproc)
sudo make install
```

### opusenc

```bash
sudo apt install opus-tools
```

### yadif

```bash
git clone https://github.com/Asd-g/yadifmod2
cd yadifmod2
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

### Amatsukaze本体のビルドとインストール

下記では、Amatsukazeを ```$HOME/Amatsukaze``` にインストールする例を示します。

```bash
git clone https://github.com/rigaya/Amatsukaze.git --recursive
cd Amatsukaze
./install_linux.sh $HOME/Amatsukaze
```

## 実行方法

### AmatsukazeServerCLI の実行

ビルドしたAmatsukazeServerCLIを下記のように実行します。

```bash
cd $HOME/Amatsukaze
./AmatsukazeServer.sh
```

### タスクの追加

```bash
cd $HOME/Amatsukaze
./exe_files/AmatsukazeAddTask -f <対象ファイル名> -o <出力フォルダ> -s <プロファイル名>
```


## 注意事項

1. **既知の制限**:
   - 一部のWindowsに依存する機能は制限または無効化されています
   - ffmpegはシステムライブラリを使用します
   - GUI機能は利用できません
   - 必ず ```dotnet publish``` して利用してください。
   