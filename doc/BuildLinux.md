# Linux向けAmatsukazeServerのビルド手順

## 必要なツールと依存パッケージのインストール

```bash
sudo apt update
sudo apt install -y build-essential git wget curl nasm cmake meson ninja-build pkg-config autoconf automake libtool
```
次に .NET 10.0 SDKをインストールします。下記はUbuntu 24.04の例を示します。その他の環境については、[リンク先](https://learn.microsoft.com/ja-jp/dotnet/core/install/linux)を参照してください。

```bash
# .NET
wget https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i ./packages-microsoft-prod.deb
sudo apt update
sudo apt install -y dotnet-sdk-10.0
dotnet workload install wasm-tools --skip-manifest-update
```

`AmatsukazeWebUI` は Blazor WebAssembly を publish するため、`wasm-tools` workload がないと
`Publishing without optimizations...` という警告が表示され、WebUI が非最適化で公開されます。

## Amatsukaze本体のビルド

下記では、Amatsukazeを ```$HOME/Amatsukaze``` にインストールする例を示します。

```./scripts/build.sh``` により下記が自動的に実行されます。

- AmatsuakzeCLI, libAmatsukaze.soのビルド (C++)
  - 依存するffmpeg関連ライブラリのビルドを含む
- AmatsuakzeServer, AmatsuakzeServerCLI, AmatsuakzeAddTask のビルド (C# dotnet)
- WebUI静的ファイルの公開と配置 (`exe_files/wwwroot`)
- インストール先への実行ファイルの配置

```bash
git clone https://github.com/rigaya/Amatsukaze.git --recursive
cd Amatsukaze
./scripts/build.sh $HOME/Amatsukaze
```

## 依存モジュールのビルド

配布用の依存モジュールは`scripts/build_dep_tools.sh`でビルドします。以下は従来の手動導入手順です。配布アーカイブを使う場合は不要です。

### AviSynth+/AvisynthCUDAFiltersのインストール

[こちら](https://github.com/rigaya/AviSynthCUDAFilters/releases)から最新版のdebパッケージをダウンロードします。なお、自ビルドする場合は[こちら](https://github.com/rigaya/AviSynthCUDAFilters/blob/master/README_LINUX.md)を参考にしてください。

CUDAを使用する場合、CUDAを有効にしてビルドした下記AviSynth+をインストールする必要があります。以下は0.7.5リリースのファイル名です。
- avisynth_3.7.5-1_amd64_linux.deb
- avisynthcudafilters_0.7.5-1_amd64_linux.deb

```bash
sudo apt install -y ./avisynth_3.7.5-1_amd64_linux.deb
sudo apt install -y ./avisynthcudafilters_0.7.5-1_amd64_linux.deb
```

<details>
<summary>最新版をすべてコマンドでインストールする場合 (クリックで展開)</summary>

```bash
(curl -s https://api.github.com/repos/rigaya/AviSynthCUDAFilters/releases/latest \
  | grep "browser_download_url.*deb" | grep "avisynth_" | grep "amd64_linux" | cut -d : -f 2,3 | tr -d \" \
  | wget -i - -O avisynth.deb \
  && sudo apt install -y ./avisynth.deb \
  && rm ./avisynth.deb)

(curl -s https://api.github.com/repos/rigaya/AviSynthCUDAFilters/releases/latest \
  | grep "browser_download_url.*deb" | grep "avisynthcudafilters_" | grep "amd64_linux" | cut -d : -f 2,3 | tr -d \" \
  | wget -i - -O avisynthcudafilters.deb \
  && sudo apt install -y ./avisynthcudafilters.deb \
  && rm ./avisynthcudafilters.deb)
```
</details>

### その他必要なAvisynthプラグインのインストール

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

- nnedi3

  ```bash
  (git clone -b avsp https://github.com/rigaya/NNEDI3.git \
    && cd NNEDI3 && mkdir build && cd build && meson setup .. \
    && ninja \
    && sudo ninja install)
  ```

- masktools

  ```bash
  (git clone https://github.com/pinterf/masktools.git \
    && cd masktools \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && sudo make install)
  ```

- mvtools

  ```bash
  (git clone https://github.com/pinterf/mvtools.git \
    && cd mvtools \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && sudo make install)
  ```

- RgTools

  ```bash
  (git clone https://github.com/pinterf/RgTools.git \
    && cd RgTools \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && sudo make install)
  ```


### 必要な実行ファイルのインストール

- エンコーダ

  - x264, x265, svt-av1
  
    新しめの実行ファイルは[こちら](https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases)からダウンロードできます。

    または、パッケージからの導入も可能です。

    ```bash
    sudo apt install -y x264 x265 svt-av1
    ```

  - x262

    ```bash
    (git clone https://code.videolan.org/videolan/x262.git \
      && cd x262 \
      && ./configure --enable-mpeg2 \
      && make -j$(nproc) \
      && sudo install -D -t /usr/local/bin x262)
    ```

- muxer

  - mp4box

    ```bash
    (sudo apt install libz-dev \
      && git clone https://github.com/gpac/gpac.git \
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

    [こちら](https://github.com/rigaya/tsreplace/releases)から最新版をダウンロードしてインストールします。

    ```bash
    sudo apt install -y ./tsreplace_<version>_amd64.deb
    ```

    <details>
    <summary>最新版をすべてコマンドでインストールする場合 (クリックで展開)</summary>

    ```bash
    (curl -s https://api.github.com/repos/rigaya/tsreplace/releases/latest \
      | grep "browser_download_url.*deb" | grep "amd64" | cut -d : -f 2,3 | tr -d \" \
      | wget -i - -O tsreplace.deb \
      && sudo apt install -y ./tsreplace.deb \
      && rm ./tsreplace.deb)
    ```
    </details>

- 字幕処理等

  - tsreadex

    ```bash
    (git clone https://github.com/xtne6f/tsreadex.git \
      && cd tsreadex \
      && make -j$(nproc) \
      && sudo install -D -t /usr/local/bin tsreadex)
    ```

  - psisiarc

    ```bash
    (git clone https://github.com/xtne6f/psisiarc.git \
    && cd psisiarc \
    && make -j$(nproc) \
    && sudo install -D -t /usr/local/bin psisiarc)
    ```

  - b24tovtt

    ```bash
    (git clone https://github.com/xtne6f/b24tovtt.git \
    && cd b24tovtt \
    && make -j$(nproc) \
    && sudo install -D -t /usr/local/bin b24tovtt)
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
    (git clone --depth=1 --branch v5.1.1 https://github.com/yobibi/join_logo_scp \
      && cd join_logo_scp/src \
      && make \
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

- リネーム

  - [SCRename.py](https://github.com/rigaya/SCRenamePy)を使用してください。(SCRename.vbsはLinuxでは使用できません)


### 各Avisynthプラグインへのリンクの作成

実際にAmatsuakzeを使用するには、各種Avisynthプラグインをインストール後、```exe_files/plugins64```にそのリンクを作成する必要があります。

```./scripts/install.sh```を実行するとインストール済みの各Avisynthプラグインへのリンクが```exe_files/plugins64```に自動的に作成されます。

```bash
cd $HOME/Amatsukaze
./scripts/install.sh
```
