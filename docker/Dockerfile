# Amatsukaze Dockerfile
# Ubuntu 24.04ベースでAmatsukazeを実行するためのマルチステージビルド
# デバッグモードとリリースモードの両方に対応

# ビルド段階
FROM ubuntu:24.04 AS builder

# 変数の定義
ARG UBUNTU_VERSION=24.04
ARG ARCH=amd64
ARG BUILD_MODE=release

# 環境変数の設定
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Tokyo
ENV UBUNTU_VERSION=${UBUNTU_VERSION}
ENV ARCH=${ARCH}
ENV BUILD_MODE=${BUILD_MODE}

# ビルドツールのインストール
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    wget \
    curl \
    nasm \
    cmake \
    meson \
    ninja-build \
    pkg-config \
    autoconf \
    automake \
    libtool \
    curl \
    wget \
    && rm -rf /var/lib/apt/lists/*

# 依存パッケージのインストール
RUN apt-get update && apt-get install -y \
    openssl \
    zlib1g \
    libz-dev \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# AviSynth+のインストール
RUN curl -s https://api.github.com/repos/rigaya/AviSynthCUDAFilters/releases/latest \
    | grep "browser_download_url.*deb" | grep "avisynth_" | grep "Ubuntu${UBUNTU_VERSION}" | grep "${ARCH}" | cut -d : -f 2,3 | tr -d \" \
    | wget -i - -O avisynth.deb \
    && apt-get install -y ./avisynth.deb \
    && rm ./avisynth.deb

# Avisynthプラグインのビルドとインストール
WORKDIR /tmp/plugins

# yadif
RUN git clone https://github.com/Asd-g/yadifmod2 \
    && cd yadifmod2 \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && make install

# TIVTC
RUN git clone https://github.com/pinterf/TIVTC \
    && cd TIVTC/src \
    && cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -B build -S . \
    && cmake --build build -j $(nproc) \
    && cd build \
    && make install

# nnedi3
RUN git clone -b avsp https://github.com/rigaya/NNEDI3.git \
    && cd NNEDI3 && mkdir build && cd build && meson setup .. \
    && ninja \
    && ninja install

# masktools
RUN git clone https://github.com/pinterf/masktools.git \
    && cd masktools \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && make install

# mvtools
RUN git clone https://github.com/pinterf/mvtools.git \
    && cd mvtools \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && make install

# RgTools
RUN git clone https://github.com/pinterf/RgTools.git \
    && cd RgTools \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && make install

# muxerのインストール（ビルドが必要なもののみ）
# mp4box
RUN git clone https://github.com/gpac/gpac.git \
    && cd gpac \
    && ./configure --static-bin \
    && make -j$(nproc) \
    && make install

# L-SMASH
RUN git clone https://github.com/l-smash/l-smash.git \
    && cd l-smash \
    && ./configure \
    && make -j$(nproc) \
    && make install

# CM/ロゴ解析ツール
# chapter_exe
RUN git clone https://github.com/rigaya/chapter_exe \
    && cd chapter_exe/src \
    && make -j$(nproc) \
    && install -D -t /usr/local/bin chapter_exe

# join_logo_scp
RUN git clone https://github.com/tobitti0/join_logo_scp \
    && cd join_logo_scp/src \
    && make -j$(nproc) \
    && install -D -t /usr/local/bin join_logo_scp

# 音声エンコーダ
# fdkaac
RUN git clone https://github.com/mstorsjo/fdk-aac.git \
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
    && make install

# opusenc
RUN apt-get update && apt-get install -y opus-tools \
    && rm -rf /var/lib/apt/lists/*

# ビルド段階のクリーンアップ
RUN rm -rf /tmp/plugins

# 実行段階
FROM nvidia/cuda:12.9.1-runtime-ubuntu24.04 AS runtime

# 変数の定義（実行段階でも使用）
ARG UBUNTU_VERSION=24.04
ARG ARCH=amd64
ARG BUILD_MODE=release

# 環境変数の設定
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Tokyo
ENV UBUNTU_VERSION=${UBUNTU_VERSION}
ENV ARCH=${ARCH}
ENV BUILD_MODE=${BUILD_MODE}
ENV LANG=ja_JP.UTF-8
ENV LC_ALL=ja_JP.UTF-8
ENV LC_CTYPE=ja_JP.UTF-8

# 実行に必要なパッケージのみインストール（デバッグモードの場合はビルドツールも含める）
RUN apt-get update && apt-get install -y \
    ca-certificates \
    gosu \
    gpg \
    curl \
    wget \
    xz-utils \
    locales \
    openssl \
    zlib1g \
    x264 \
    x265 \
    svt-av1 \
    mkvtoolnix \
    opus-tools \
    && if [ "$BUILD_MODE" = "debug" ]; then \
        apt-get install -y \
        gosu \
        build-essential \
        git \
        nasm \
        cmake \
        meson \
        ninja-build \
        pkg-config \
        autoconf \
        automake \
        libtool \
        libssl-dev \
        libz-dev; \
    fi \
    && rm -rf /var/lib/apt/lists/*

# 日本語ロケールの設定
RUN locale-gen ja_JP.UTF-8 \
    && locale-gen en_US.UTF-8 \
    && update-locale LANG=ja_JP.UTF-8

# .NET SDK 8.0のインストール（デバッグモードの場合のみ）
RUN if [ "$BUILD_MODE" = "debug" ]; then \
        wget https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb \
        && dpkg -i ./packages-microsoft-prod.deb \
        && rm ./packages-microsoft-prod.deb \
        && apt-get update \
        && apt-get install -y dotnet-sdk-8.0 \
        && rm -rf /var/lib/apt/lists/*; \
    fi

# AviSynth+/AvisynthCUDAFiltersのインストール（実行段階でも必要）
RUN curl -s https://api.github.com/repos/rigaya/AviSynthCUDAFilters/releases/latest \
    | grep "browser_download_url.*deb" | grep "avisynth_" | grep "Ubuntu${UBUNTU_VERSION}" | grep "${ARCH}" | cut -d : -f 2,3 | tr -d \" \
    | wget -i - -O avisynth.deb \
    && apt-get install -y ./avisynth.deb \
    && rm ./avisynth.deb

RUN curl -s https://api.github.com/repos/rigaya/AviSynthCUDAFilters/releases/latest \
    | grep "browser_download_url.*deb" | grep "avisynthcudafilters_" | grep "Ubuntu${UBUNTU_VERSION}" | grep "${ARCH}" | cut -d : -f 2,3 | tr -d \" \
    | wget -i - -O avisynthcudafilters.deb \
    && apt-get install -y ./avisynthcudafilters.deb \
    && rm ./avisynthcudafilters.deb

# Intel GPU用ドライバインストール
RUN curl -fsSL https://repositories.intel.com/gpu/intel-graphics.key | gpg --dearmor -o /usr/share/keyrings/intel-graphics-keyring.gpg && \
    apt-get update && apt-get upgrade -y && \
    apt-get install -y --no-install-recommends

# debファイルからインストールするエンコーダ
# qsvencc, nvencc, vceenccのインストール
RUN curl -s https://api.github.com/repos/rigaya/QSVEnc/releases/latest \
    | grep "browser_download_url.*deb" | grep "Ubuntu${UBUNTU_VERSION}" | grep "${ARCH}" | cut -d : -f 2,3 | tr -d \" \
    | wget -i - -O qsvencc.deb \
    && apt-get install -y ./qsvencc.deb \
    && rm ./qsvencc.deb

RUN curl -s https://api.github.com/repos/rigaya/NVEnc/releases/latest \
    | grep "browser_download_url.*deb" | grep "Ubuntu${UBUNTU_VERSION}" | grep "${ARCH}" | cut -d : -f 2,3 | tr -d \" \
    | wget -i - -O nvencc.deb \
    && apt-get install -y ./nvencc.deb \
    && rm ./nvencc.deb

#RUN curl -s https://api.github.com/repos/rigaya/VCEEnc/releases/latest \
#    | grep "browser_download_url.*deb" | grep "Ubuntu${UBUNTU_VERSION}" | grep "${ARCH}" | cut -d : -f 2,3 | tr -d \" \
#    | wget -i - -O vceencc.deb \
#    && apt-get install -y ./vceencc.deb \
#    && rm ./vceencc.deb

# tsreplace
RUN curl -s https://api.github.com/repos/rigaya/tsreplace/releases/latest \
    | grep "browser_download_url.*deb" | grep "Ubuntu${UBUNTU_VERSION}" | grep "${ARCH}" | cut -d : -f 2,3 | tr -d \" \
    | wget -i - -O tsreplace.deb \
    && apt-get install -y ./tsreplace.deb \
    && rm ./tsreplace.deb

# ビルド段階から必要なファイルをコピー
COPY --from=builder /usr/local/lib /usr/local/lib
COPY --from=builder /usr/local/bin /usr/local/bin
COPY --from=builder /usr/lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu

# ライブラリパスの設定
ENV LD_LIBRARY_PATH=/usr/local/lib:/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

# 作業ディレクトリの設定
WORKDIR /app

# ディレクトリ構造の作成
RUN mkdir -p /app/bat /app/config /app/exe_files/plugins64 /app/input /app/logo /app/output /app/profile /app/temp

# Amatsukazeのインストール（モードに応じて切り替え）
RUN if [ "$BUILD_MODE" = "debug" ]; then \
        # デバッグモード: ソースコードをクローン
        git clone -b linux http://192.168.0.24:8080/git/rigaya/Amatsukaze.git --recursive /app/Amatsukaze-src; \
    else \
        # リリースモード: 指定されたUbuntuバージョン用のアーカイブを自動ダウンロード
        curl -s https://api.github.com/repos/rigaya/Amatsukaze/releases/latest \
        | grep "browser_download_url.*tar.xz" | grep "Ubuntu${UBUNTU_VERSION}" | cut -d : -f 2,3 | tr -d \" \
        | wget -i - -O - | tar -xJ -C /app \
        && if [ -f /app/scripts/install.sh ]; then /app/scripts/install.sh; fi; \
    fi

# for debug
#COPY Amatsukaze_Ubuntu24.04_r815.tar.xz /app
#RUN tar -xJf Amatsukaze_Ubuntu24.04_r815.tar.xz -C /app

# デバッグ用のシェルスクリプトを作成（デバッグモードの場合のみ）
RUN if [ "$BUILD_MODE" = "debug" ]; then \
        echo '#!/bin/bash' > /app/debug-build.sh && \
        echo 'cd /app/Amatsukaze-src' >> /app/debug-build.sh && \
        echo './scripts/build.sh /app build --debug' >> /app/debug-build.sh && \
        echo 'if [ -f /app/scripts/install.sh ]; then /app/scripts/install.sh; fi' >> /app/debug-build.sh && \
        chmod +x /app/debug-build.sh; \
    fi

# エントリーポイントスクリプトを作成
RUN echo '#!/bin/bash' > /app/entrypoint.sh && \
    echo 'chown -R "$UID:$GID" /app' >> /app/entrypoint.sh && \
    echo 'exec gosu "$UID:$GID" "$@"' >> /app/entrypoint.sh && \
    chmod +x /app/entrypoint.sh

# デフォルトコマンド（モードに応じて切り替え）
ENTRYPOINT ["./entrypoint.sh"]
CMD if [ "$BUILD_MODE" = "debug" ]; then /bin/bash; else ./AmatsukazeServer.sh; fi
