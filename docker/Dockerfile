# Amatsukaze Dockerfile
# Ubuntu 22.04ベースでAmatsukazeを実行するためのマルチステージビルド
# デバッグモードとリリースモードの両方に対応

# ビルド段階
FROM ubuntu:22.04 AS builder

# 変数の定義
ARG UBUNTU_VERSION=22.04
ARG ARCH=amd64
ARG BUILD_MODE=release

# 環境変数の設定
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Tokyo
ENV UBUNTU_VERSION=${UBUNTU_VERSION}
ENV ARCH=${ARCH}
ENV BUILD_MODE=${BUILD_MODE}

# バージョン定義
ENV AVISYNTHCUDAFILTERS_VER=0.7.1
ENV AVISYNTH_VER=3.7.5
ENV YADIFMOD2_VER=0.2.8
ENV TIVTC_VER=v1.0.29
ENV NNEDI3_REV=a93dbaea9f0dfc3f6d496a3fe01466bc22dd3a88
ENV MASKTOOLS_REV=8291927bf6956981a6412d353da8ca39d49c9d3a
ENV MVTOOLS_VER=2.7.46
ENV RGTOOLS_VER=1.2
ENV GPAC_VER=v2.4.0
ENV LSMASH_REV=18a9ed25c7ff79a7f4f4bf850c345c72179b8998
ENV CHAPTER_EXE_REV=32880d45f088e574285a101e6a49b032bb04f6ea
ENV JOIN_LOGO_SCP_VER=Ver4.1.0_Linux
ENV FDK_AAC_REV=d8e6b1a3aa606c450241632b64b703f21ea31ce3
ENV FDKAAC_VER=v1.0.6

# ビルドツールのインストール
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    curl \
    wget \
    p7zip-full \
    nasm \
    cmake \
    meson \
    ninja-build \
    pkg-config \
    autoconf \
    automake \
    libtool \
    openssl \
    zlib1g \
    libz-dev \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/* \
    && apt-get clean \
    && apt-get autoremove -y

# AviSynth+のインストール
RUN wget https://github.com/rigaya/AviSynthCUDAFilters/releases/download/${AVISYNTHCUDAFILTERS_VER}/avisynth_${AVISYNTH_VER}-1_${ARCH}_Ubuntu${UBUNTU_VERSION}.deb -O avisynth.deb \
    && apt-get install -y ./avisynth.deb \
    && rm ./avisynth.deb

# Avisynthプラグインのビルドとインストール
WORKDIR /tmp/plugins

# yadif
RUN git clone --depth=1 --branch ${YADIFMOD2_VER} https://github.com/Asd-g/yadifmod2 \
    && cd yadifmod2 \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && make install

# TIVTC
RUN git clone --depth=1 --branch ${TIVTC_VER} https://github.com/pinterf/TIVTC \
    && cd TIVTC/src \
    && cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -B build -S . \
    && cmake --build build -j $(nproc) \
    && cd build \
    && make install

# nnedi3
RUN git clone -b avsp --single-branch https://github.com/rigaya/NNEDI3.git \
    && cd NNEDI3 \
    && git checkout ${NNEDI3_REV} \
    && mkdir build && cd build && meson setup .. \
    && ninja \
    && ninja install

# masktools
RUN git clone https://github.com/pinterf/masktools.git \
    && cd masktools \
    && git checkout ${MASKTOOLS_REV} \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && make install

# mvtools
RUN git clone --depth=1 --branch ${MVTOOLS_VER} https://github.com/pinterf/mvtools.git \
    && cd mvtools \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && make install

# RgTools
RUN git clone --depth=1 --branch ${RGTOOLS_VER} https://github.com/pinterf/RgTools.git \
    && cd RgTools \
    && mkdir build && cd build && cmake .. \
    && make -j$(nproc) \
    && make install

# muxerのインストール（ビルドが必要なもののみ）
# mp4box
RUN git clone --depth=1 --branch ${GPAC_VER} https://github.com/gpac/gpac.git \
    && cd gpac \
    && ./configure --static-bin \
    && make -j$(nproc) \
    && make install

# L-SMASH
RUN git clone https://github.com/l-smash/l-smash.git \
    && cd l-smash \
    && git checkout ${LSMASH_REV} \
    && ./configure \
    && make -j$(nproc) \
    && make install

# CM/ロゴ解析ツール
# chapter_exe
RUN git clone https://github.com/rigaya/chapter_exe \
    && cd chapter_exe \
    && git checkout ${CHAPTER_EXE_REV} \
    && cd src \
    && make -j$(nproc) \
    && install -D -t /usr/local/bin chapter_exe

# join_logo_scp
RUN git clone --depth=1 --branch ${JOIN_LOGO_SCP_VER} https://github.com/tobitti0/join_logo_scp \
    && cd join_logo_scp/src \
    && make -j$(nproc) \
    && install -D -t /usr/local/bin join_logo_scp

# 音声エンコーダ
# fdkaac
RUN git clone https://github.com/mstorsjo/fdk-aac.git \
    && cd fdk-aac \
    && git checkout ${FDK_AAC_REV} \
    && ./autogen.sh \
    && ./configure --disable-shared --prefix=$(pwd)/fdk-aac-libs \
    && make -j$(nproc) \
    && make install \
    && cd .. \
    && git clone --depth=1 --branch ${FDKAAC_VER} https://github.com/nu774/fdkaac.git \
    && cd fdkaac \
    && autoreconf -i \
    && PKG_CONFIG_PATH=../fdk-aac/fdk-aac-libs/lib/pkgconfig ./configure \
    && make -j$(nproc) \
    && make install

# ビルド段階のクリーンアップ
RUN rm -rf /tmp/plugins \
    && rm -rf /var/cache/apt/* \
    && rm -rf /var/lib/apt/lists/* \
    && rm -rf /usr/share/doc/* \
    && rm -rf /usr/share/man/* \
    && rm -rf /usr/share/locale/* \
    && rm -rf /usr/share/info/*

# 実行段階
FROM nvidia/cuda:11.8.0-runtime-ubuntu22.04 AS runtime

# 変数の定義（実行段階でも使用）
ARG UBUNTU_VERSION=22.04
ARG UBUNTU_NAME=jammy
ARG ARCH=amd64
ARG BUILD_MODE=release

# 環境変数の設定
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Tokyo
ENV UBUNTU_VERSION=${UBUNTU_VERSION}
ENV UBUNTU_NAME=${UBUNTU_NAME}
ENV ARCH=${ARCH}
ENV BUILD_MODE=${BUILD_MODE}
ENV LANG=ja_JP.UTF-8
ENV LC_ALL=ja_JP.UTF-8
ENV LC_CTYPE=ja_JP.UTF-8

ENV QSVENCC_VER=8.01
ENV NVENCC_VER=9.04
ENV VCEENCC_VER=9.01
ENV TSREPLACE_VER=0.16
ENV AVISYNTHCUDAFILTERS_VER=0.7.3
ENV AVISYNTH_VER=3.7.5

# 実行に必要なパッケージのみインストール（デバッグモードの場合はビルドツールも含める）
RUN apt-get update && apt-get install -y \
    gpg \
    curl \
    wget \
    software-properties-common \
    && wget -qO - https://repositories.intel.com/gpu/intel-graphics.key | gpg --yes --dearmor --output /usr/share/keyrings/intel-graphics.gpg \
    && echo "deb [arch=amd64,i386 signed-by=/usr/share/keyrings/intel-graphics.gpg] https://repositories.intel.com/gpu/ubuntu ${UBUNTU_NAME} unified" \
      | tee /etc/apt/sources.list.d/intel-gpu-${UBUNTU_NAME}.list \
    && curl -fsSL https://repo.radeon.com/rocm/rocm.gpg.key | gpg --dearmor -o /usr/share/keyrings/rocm-keyring.gpg \
    && echo 'deb [arch=amd64 signed-by=/usr/share/keyrings/rocm-keyring.gpg] https://repo.radeon.com/amdgpu/5.7.1/ubuntu/ jammy main' > /etc/apt/sources.list.d/amdgpu.list \
    && echo 'deb [arch=amd64 signed-by=/usr/share/keyrings/rocm-keyring.gpg] https://repo.radeon.com/amdgpu/5.7.1/ubuntu/ jammy proprietary' > /etc/apt/sources.list.d/amdgpu-proprietary.list \
    && echo 'deb [arch=amd64 signed-by=/usr/share/keyrings/rocm-keyring.gpg] https://repo.radeon.com/rocm/apt/5.7.1 jammy main' > /etc/apt/sources.list.d/rocm.list \
    && apt-get update && apt-get upgrade -y \
    ca-certificates \
    gosu \
    git \
    xz-utils \
    locales \
    openssl \
    zlib1g \
    mkvtoolnix \
    opus-tools \
    intel-media-va-driver-non-free intel-opencl-icd libigfxcmrt7 libmfx1 libmfx-gen1.2 libva-drm2 libva-x11-2 ocl-icd-opencl-dev \
    && wget https://github.com/rigaya/QSVEnc/releases/download/${QSVENCC_VER}/qsvencc_${QSVENCC_VER}_Ubuntu${UBUNTU_VERSION}_${ARCH}.deb -O qsvencc.deb \
        && apt-get install -y ./qsvencc.deb \
        && rm ./qsvencc.deb \
    && wget https://github.com/rigaya/NVEnc/releases/download/${NVENCC_VER}/nvencc_${NVENCC_VER}_Ubuntu${UBUNTU_VERSION}_${ARCH}.deb -O nvencc.deb \
        && apt-get install -y ./nvencc.deb \
        && rm ./nvencc.deb \
    && wget https://github.com/rigaya/VCEEnc/releases/download/${VCEENCC_VER}/vceencc_${VCEENCC_VER}_Ubuntu${UBUNTU_VERSION}_${ARCH}.deb -O vceencc.deb \
        && apt-get install -y ./vceencc.deb \
        && rm ./vceencc.deb \
    && wget https://github.com/rigaya/tsreplace/releases/download/${TSREPLACE_VER}/tsreplace_${TSREPLACE_VER}_Ubuntu${UBUNTU_VERSION}_${ARCH}.deb -O tsreplace.deb \
        && apt-get install -y ./tsreplace.deb \
        && rm ./tsreplace.deb \
    && if [ "$BUILD_MODE" = "debug" ]; then \
        apt-get install -y \
        software-properties-common \
        build-essential \
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
    && rm -rf /var/lib/apt/lists/* \
    && apt-get clean \
    && apt-get autoremove -y

# 日本語ロケールの設定
RUN locale-gen ja_JP.UTF-8 \
    && locale-gen en_US.UTF-8 \
    && update-locale LANG=ja_JP.UTF-8

# .NET SDK 9.0のインストール（デバッグモードの場合のみ）
RUN if [ "$BUILD_MODE" = "debug" ]; then \
        apt add-apt-repository ppa:dotnet/backports \
        && apt update \
        && apt install -y dotnet-sdk-9.0 \
        && rm -rf /var/lib/apt/lists/*; \
    fi

# AviSynth+/AvisynthCUDAFiltersのインストール（実行段階でも必要）
RUN wget https://github.com/rigaya/AviSynthCUDAFilters/releases/download/${AVISYNTHCUDAFILTERS_VER}/avisynth_${AVISYNTH_VER}-1_${ARCH}_Ubuntu${UBUNTU_VERSION}.deb -O avisynth.deb \
    && apt-get install -y ./avisynth.deb \
    && rm ./avisynth.deb \
    && wget https://github.com/rigaya/AviSynthCUDAFilters/releases/download/${AVISYNTHCUDAFILTERS_VER}/avisynthcudafilters_${AVISYNTHCUDAFILTERS_VER}-1_${ARCH}_Ubuntu${UBUNTU_VERSION}.deb -O avisynthcudafilters.deb \
    && apt-get install -y ./avisynthcudafilters.deb \
    && rm ./avisynthcudafilters.deb

# ライブラリパスの設定
ENV LD_LIBRARY_PATH=/usr/local/lib:/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

# 作業ディレクトリの設定
WORKDIR /app

# ディレクトリ構造の作成
RUN mkdir -p /app/bat /app/config /app/exe_files/plugins64 /app/recorded /app/logo /app/output /app/profile /app/temp

# SCRename.py
RUN git clone --depth 1 -b 0.04 https://github.com/rigaya/SCRenamePy.git tmp_SCRenamePy \
    && mkdir -p exe_files/SCRenamePy \
    && mv tmp_SCRenamePy/SCRename.* exe_files/SCRenamePy/ \
    && rm -rf tmp_SCRenamePy

RUN wget https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/download/auto-build-20250927-083302/x264_3223_amd64_linux.tar.xz -O x264.tar.xz \
    && wget https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/download/auto-build-20250927-083302/x265_4.1+191_amd64_linux.tar.xz -O x265.tar.xz \
    && wget https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/download/auto-build-20251004-072930/SvtAv1EncApp_3.1.0-150_amd64_linux.tar.xz -O svt-av1.tar.xz \
    && tar -xf x264.tar.xz -C /app/exe_files \
    && tar -xf x265.tar.xz -C /app/exe_files \
    && tar -xf svt-av1.tar.xz -C /app/exe_files \
    && rm x264.tar.xz x265.tar.xz svt-av1.tar.xz

# ビルド段階から必要なファイルをコピー
COPY --from=builder /usr/local/lib /usr/local/lib
COPY --from=builder /usr/local/bin /usr/local/bin
COPY --from=builder /usr/lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu

# Amatsukazeのインストール
RUN curl -s https://api.github.com/repos/rigaya/Amatsukaze/releases/latest \
        | grep "browser_download_url.*tar.xz" | grep "Ubuntu${UBUNTU_VERSION}" | cut -d : -f 2,3 | tr -d \" \
        | wget -i - -O - | tar -xJ -C /app \
        && if [ -f /app/scripts/install.sh ]; then /app/scripts/install.sh; fi

# for debug
#COPY Amatsukaze_Ubuntu22.04_r845.tar.xz /app
#RUN tar -xJf Amatsukaze_Ubuntu22.04_r845.tar.xz -C /app \
#    && if [ -f /app/scripts/install.sh ]; then /app/scripts/install.sh; fi \
#    && rm Amatsukaze_Ubuntu22.04_r845.tar.xz

# デバッグ用のシェルスクリプトを作成（デバッグモードの場合のみ）
RUN if [ "$BUILD_MODE" = "debug" ]; then \
        echo '#!/bin/bash' > /app/debug-build.sh && \
        echo 'cd /app/Amatsukaze-src' >> /app/debug-build.sh && \
        echo './scripts/build.sh /app build --debug' >> /app/debug-build.sh && \
        echo 'if [ -f /app/scripts/install.sh ]; then /app/scripts/install.sh; fi' >> /app/debug-build.sh && \
        chmod +x /app/debug-build.sh; \
    fi

# エントリーポイントスクリプトを作成
RUN echo '#!/bin/bash' > /app/entrypoint.sh \
    && echo 'set -e' >> /app/entrypoint.sh \
    && echo 'DEFAULT_UID=${RUN_UID:-1000}; DEFAULT_GID=${RUN_GID:-1000};' >> /app/entrypoint.sh \
    && echo 'if ! getent group "$DEFAULT_GID" >/dev/null 2>&1; then groupadd -g "$DEFAULT_GID" appgroup; fi' >> /app/entrypoint.sh \
    && echo 'ls -l /dev/dri/' >> /app/entrypoint.sh \
    && echo 'if [ -c /dev/dri/renderD128 ]; then' >> /app/entrypoint.sh \
    && echo '  RENDER_GID=$(stat -c "%g" /dev/dri/renderD128 2>/dev/null || echo "")' >> /app/entrypoint.sh \
    && echo '  echo "RENDER_GID: $RENDER_GID"' >> /app/entrypoint.sh \
    && echo '  if [ -n "$RENDER_GID" ] && [ "$RENDER_GID" != "" ]; then' >> /app/entrypoint.sh \
    && echo '    if ! getent group "$RENDER_GID" >/dev/null 2>&1; then groupadd -g "$RENDER_GID" render; fi' >> /app/entrypoint.sh \
    && echo '  fi' >> /app/entrypoint.sh \
    && echo 'fi' >> /app/entrypoint.sh \
    && echo 'if ! getent passwd "$DEFAULT_UID" >/dev/null 2>&1; then useradd -u "$DEFAULT_UID" -g "$DEFAULT_GID" -m -s /bin/bash appuser; fi' >> /app/entrypoint.sh \
    && echo 'chown -R "$DEFAULT_UID:$DEFAULT_GID" /app' >> /app/entrypoint.sh \
    && echo 'usermod -aG render,video `id -nu ${DEFAULT_UID}`' >> /app/entrypoint.sh \
    && echo 'exec gosu "$DEFAULT_UID" "$@"' >> /app/entrypoint.sh \
    && chmod +x /app/entrypoint.sh

# デフォルトコマンド（モードに応じて切り替え）
ENTRYPOINT ["./entrypoint.sh"]
CMD if [ "$BUILD_MODE" = "debug" ]; then /bin/bash; else ./AmatsukazeServer.sh; fi
