# Linux向けAmatsukazeServer

**注意**
現在建設中でたぶん対応しきれていない箇所が多く、バグだらけです。

## 概要

AmatsukazeServerCLI と AmatsukazeCLI、AmatsukazeAddTask をLinux対応作業中です。

AmatsukazeGUIは.NETのWPFが使われており、WPFはLinuxに対応していないようなので、Linux対応は難しいです。

そのため、LinuxでAmatsukazeServerCLIを起動して、WindowsからAmatsukazeGUIで接続する形を想定しています。

また、タスクのキューへの追加はAmatsukazeAddTaskの利用を想定しています。

## 想定動作環境

- x64のLinux環境

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
    - Windows-Linuxの対応関係を設定できればあるいは…?

- Linux対応困難
  - AmatsukazeGUI
  - エンコード中の一時停止

- その他対応予定なし
  - エンコード後、スリープ・シャットダウン
  - スレッドアフィニティの指定
  - ffmpegに対する独自拡張の取り込み
  - 他のエンコーダの追加等
  - SCRename によるリネーム機能


## インストール手順

### 依存パッケージのインストール

#### Ubuntu / Debian系

```bash
sudo apt update
sudo apt install -y build-essential git cmake meson ninja-build pkg-config \
    autoconf automake libtool \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    libssl-dev libz-dev

# .NET
wget https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i ./packages-microsoft-prod.deb
sudo apt update
sudo apt install -y dotnet-sdk-8.0
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

### 必要な実行ファイルとプラグインのインストール

- エンコーダ

  - x264, x265, svt-av1
  
    ```bash
    sudo apt install x264 x265 svt-av1
    ```
  
  - qsvencc, nvencc, vceencc
  
    - ドライバも含めたインストール方法
      - [qsvencc](https://github.com/rigaya/QSVEnc/blob/master/Install.ja.md) ([ダウンロード先](https://github.com/rigaya/QSVEnc/releases))
      - [nvencc](https://github.com/rigaya/NVEnc/blob/master/Install.ja.md)  ([ダウンロード先](https://github.com/rigaya/NVEnc/releases))
      - [vceencc](https://github.com/rigaya/VCEEnc/blob/master/Install.ja.md) ([ダウンロード先](https://github.com/rigaya/VCEEnc/releases))
  
    ```bash
    # qsvencc
    sudo apt install ./qsvencc_x.xx_Ubuntu2x.04_amd64.deb
    
    # nvencc
    sudo apt install ./nvencc_x.xx_Ubuntu2x.04_amd64.deb
    
    # vceencc
    sudo apt install ./vceencc_x.xx_Ubuntu2x.04_amd64.deb
    ```

- muxer

  - mp4box
  
    ```bash
    git clone https://github.com/gpac/gpac.git
    cd gpac
    ./configure --static-bin
    make -j$(nproc)
    sudo make install
    ```
  
  - mkvmerge
  
    ```bash
    sudo apt install mkvtoolnix
    ```
  
  - L-SMASH (muxer, timelineeditor)
  
    ```bash
    git clone https://github.com/l-smash/l-smash.git
    cd l-smash/
    ./configure
    make -j$(nproc)
    sudo make install
    ```

  - tsreplace

    [ダウンロード先](https://github.com/rigaya/tsreplace/releases)

    ```bash
    sudo apt install ./tsreplace_x.xx_Ubuntu2x.04_amd64.deb
    ```

- CM/ロゴ解析等

  - chapter_exe
  
    ```bash
    git clone https://github.com/tobitti0/chapter_exe
    cd chapter_exe/src
    sudo install -D -t /usr/local/bin chapter_exe
    ```
  
  - join_logo_scp
  
    ```bash
    git clone https://github.com/tobitti0/join_logo_scp
    cd join_logo_scp/src
    sudo install -D -t /usr/local/bin join_logo_scp
    ```

- 音声エンコーダ

  - fdkaac
  
    ```bash
    git clone https://github.com/mstorsjo/fdk-aac.git
    cd fdk-aac
    ./autogen.sh
    ./configure --disable-shared
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
  
  - opusenc
  
    ```bash
    sudo apt install opus-tools
    ```

- Avisynthプラグイン

  - yadif
  
    ```bash
    git clone https://github.com/Asd-g/yadifmod2
    cd yadifmod2
    mkdir build && cd build
    cmake ..
    make -j$(nproc)
    sudo make install
    ```
  
  - TIVTC
  
    ```bash
    git clone https://github.com/pinterf/TIVTC
    cd TIVTC/src
    cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -B build -S .
    cmake --build build
    sudo make install
    ```

### Amatsukaze本体のビルドとインストール

下記では、Amatsukazeを ```$HOME/Amatsukaze``` にインストールする例を示します。

```./install_linux.sh``` により下記が自動的に実行されます。

- AmatsuakzeCLIのビルド
- AmatsuakzeServer, AmatsuakzeServerCLI, AmatsuakzeAddTask のビルド
- インストール先への実行ファイルの配置
- yadif, TIVTC 等プラグインの exe_files/plugins64 へのリンク作成
  

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
   - 一部のWindowsに依存する機能は制限または無効化されています。
   - ffmpegはシステムライブラリを使用します。
   - GUI機能はLinuxでは利用できません。
   - 必ず ```dotnet publish``` して利用してください。
   - ロゴ解析は、AmatsukazeGUI(Windows側)で行います。tsファイルの場所をWindows側で選択できるようにしてください。
   