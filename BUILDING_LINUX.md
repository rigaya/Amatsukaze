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
  - ffmpegに対する独自拡張の取り込み
  - 他のエンコーダの追加等
  - SCRename によるリネーム機能


## インストール手順

### 依存パッケージのインストール

#### Ubuntu / Debian系

```bash
sudo apt update
sudo apt install -y build-essential git wget curl nasm cmake meson ninja-build pkg-config \
    autoconf automake libtool \
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
(git clone https://github.com/AviSynth/AviSynthPlus.git
  && cd AviSynthPlus && mkdir build && cd build
  && cmake -DENABLE_CUDA=ON ..
  && make -j$(nproc)
  && sudo make install)
```

### 必要な実行ファイルとプラグインのインストール

- エンコーダ

  - x264, x265, svt-av1
  
    ```bash
    sudo apt install -y x264 x265 svt-av1
    ```
  
  - qsvencc, nvencc, vceencc
  
    - ドライバも含めたインストール方法
      - [qsvencc](https://github.com/rigaya/QSVEnc/blob/master/Install.ja.md)
      - [nvencc](https://github.com/rigaya/NVEnc/blob/master/Install.ja.md)
      - [vceencc](https://github.com/rigaya/VCEEnc/blob/master/Install.ja.md)

    - 最新版のパッケージインストールは下記

      Ubuntu24.04 のところは対象OSにあわせて適宜置き換えてください。
  
    ```bash
    # qsvencc
    (curl -s https://api.github.com/repos/rigaya/QSVEnc/releases/latest \
      | grep "browser_download_url.*deb" | grep "Ubuntu24.04" | grep "amd64" | cut -d : -f 2,3 | tr -d \" \
      | wget -i - -O qsvencc.deb \
      && sudo apt install -y ./qsvencc.deb \
      && rm ./qsvencc.deb)
    
    # nvencc
    (curl -s https://api.github.com/repos/rigaya/NVEnc/releases/latest \
      | grep "browser_download_url.*deb" | grep "Ubuntu24.04" | grep "amd64" | cut -d : -f 2,3 | tr -d \" \
      | wget -i - -O nvencc.deb \
      && sudo apt install -y ./nvencc.deb \
      && rm ./nvencc.deb)
    
    # vceencc
    (curl -s https://api.github.com/repos/rigaya/VCEEnc/releases/latest \
      | grep "browser_download_url.*deb" | grep "Ubuntu24.04" | grep "amd64" | cut -d : -f 2,3 | tr -d \" \
      | wget -i - -O vceencc.deb \
      && sudo apt install -y ./vceencc.deb \
      && rm ./vceencc.deb)
    ```

- muxer

  - mp4box
  
    ```bash
    (git clone https://github.com/gpac/gpac.git \
      && cd gpac \
      && ./configure --static-bin \
      && make -j$(nproc) \
      && sudo make install)
    ```
  
  - mkvmerge
  
    ```bash
    sudo apt install mkvtoolnix
    ```
  
  - L-SMASH (muxer, timelineeditor)
  
    ```bash
    (git clone https://github.com/l-smash/l-smash.git \
      && cd l-smash \
      && ./configure \
      && make -j$(nproc) \
      && sudo make install)
    ```

  - tsreplace

    ```bash
    (curl -s https://api.github.com/repos/rigaya/tsreplace/releases/latest \
      | grep "browser_download_url.*deb" | grep "Ubuntu24.04" | grep "amd64" | cut -d : -f 2,3 | tr -d \" \
      | wget -i - -O tsreplace.deb \
      && sudo apt install -y ./tsreplace.deb \
      && rm ./tsreplace.deb)
    ```

- CM/ロゴ解析等

  - chapter_exe
  
    ```bash
    (git clone https://github.com/rigaya/chapter_exe \
      && cd chapter_exe/src \
      && make -j$(nproc) \
      && sudo install -D -t /usr/local/bin chapter_exe)
    ```
  
  - join_logo_scp
  
    ```bash
    (git clone https://github.com/tobitti0/join_logo_scp \
      && cd join_logo_scp/src \
      && make -j$(nproc) \
      && sudo install -D -t /usr/local/bin join_logo_scp)
    ```

- 音声エンコーダ

  - fdkaac
  
    ```bash
    (git clone https://github.com/mstorsjo/fdk-aac.git \
      && cd fdk-aac \
      && ./autogen.sh \
      && ./configure --disable-shared --prefix=$(pwd)/fdk-aac-libs \
      && make -j$(nproc) \
      && make install \
      && cd .. \
      && git clone https://github.com/nu774/fdkaac.git \
      && cd fdkaac \
      && autoreconf -i \
      && PKG_CONFIG_PATH=../fdk-aac/fdk-aac-libs/lib/pkgconfig ./configure \
      && make -j$(nproc) \
      && sudo make install)
    ```
  
  - opusenc
  
    ```bash
    sudo apt install -y opus-tools
    ```

- Avisynthプラグイン

  - yadif
  
    ```bash
    (git clone https://github.com/Asd-g/yadifmod2 \
      && cd yadifmod2 \
      && mkdir build && cd build && cmake .. \
      && make -j$(nproc) \
      && sudo make install)
    ```
  
  - TIVTC
  
    ```bash
    (git clone https://github.com/pinterf/TIVTC \
      && cd TIVTC/src \
      && cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -B build -S . \
      && cmake --build build \
      && sudo make install)
    ```

### Amatsukaze本体のビルドとインストール

下記では、Amatsukazeを ```$HOME/Amatsukaze``` にインストールする例を示します。

```./build_install_linux.sh``` により下記が自動的に実行されます。

- AmatsuakzeCLIのビルド
  - 地デジ/BS用 libAmatsukaze.so
  - BS4K用 libAmatsukaze2.so
- AmatsuakzeServer, AmatsuakzeServerCLI, AmatsuakzeAddTask のビルド
- インストール先への実行ファイルの配置

```bash
git clone https://github.com/rigaya/Amatsukaze.git --recursive
cd Amatsukaze
./build_install_linux.sh $HOME/Amatsukaze
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
   