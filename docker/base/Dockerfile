# syntax=docker/dockerfile:1.6
ARG UBUNTU_VERSION=22.04
FROM ubuntu:${UBUNTU_VERSION}

ARG DEBIAN_FRONTEND=noninteractive
ARG UBUNTU_NAME=jammy

# Ubuntu 20.04 には .NET 10 SDK が提供されないため、ネイティブ側専用のビルド環境として作る。
# その場合は build.sh を --native-only で呼ぶ前提となる。
ARG INSTALL_DOTNET=1

# Ubuntu 20.04 の apt 版 meson (0.53) / cmake (3.16) は古すぎて依存のビルドが通らないため、
# pip 版を使う。新しいディストリではそのまま apt 版でよい。
ARG PIP_BUILD_TOOLS=0
ARG MESON_VERSION=1.11.2
ARG CMAKE_VERSION=3.31.6

# 空でなければ g++-<version> を導入して既定のコンパイラにする。
# Ubuntu 20.04 の libstdc++6 は gcc 10.5 由来なので、gcc 10 を使ってもABIの下限は上がらない。
ARG GCC_VERSION=

ENV LANG=ja_JP.UTF-8
ENV LC_ALL=ja_JP.UTF-8
ENV LC_CTYPE=ja_JP.UTF-8

RUN apt update \
    && apt-get install -y \
      sudo \
      wget \
      curl \
      git \
      ca-certificates \
      software-properties-common \
      nasm \
      yasm \
      ninja-build \
      pkg-config \
      patchelf \
      ruby \
      rake \
      docbook-xsl \
      xsltproc \
      checkinstall \
      autoconf \
      automake \
      libtool \
      build-essential \
      locales \
    && if [ "${PIP_BUILD_TOOLS}" = "1" ]; then \
         apt-get install -y python3-pip \
         && pip3 install --no-cache-dir "meson==${MESON_VERSION}" "cmake==${CMAKE_VERSION}" ; \
       else \
         apt-get install -y meson cmake ; \
       fi \
    && if [ -n "${GCC_VERSION}" ]; then \
         apt-get install -y "gcc-${GCC_VERSION}" "g++-${GCC_VERSION}" \
         && update-alternatives --install /usr/bin/gcc gcc "/usr/bin/gcc-${GCC_VERSION}" 100 \
              --slave /usr/bin/g++ g++ "/usr/bin/g++-${GCC_VERSION}" \
         && update-alternatives --install /usr/bin/cc  cc  "/usr/bin/gcc-${GCC_VERSION}" 100 \
         && update-alternatives --install /usr/bin/c++ c++ "/usr/bin/g++-${GCC_VERSION}" 100 ; \
       fi \
    && locale-gen ja_JP.UTF-8 \
    && locale-gen en_US.UTF-8 \
    && update-locale LANG=ja_JP.UTF-8 \
    && if [ "${INSTALL_DOTNET}" = "1" ]; then \
         add-apt-repository ppa:dotnet/backports \
         && apt update \
         && apt-get install -y dotnet-sdk-10.0 \
         && dotnet workload install wasm-tools --skip-manifest-update ; \
       fi \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# /amt に依存をプリインストール（--deps-only）
RUN mkdir -p /amt
RUN --mount=type=bind,source=.,target=/src,readonly bash -lc 'mkdir -p /tmp/work && cd /tmp/work && /src/scripts/build.sh /amt --deps-only && touch /amt/.ready && rm -rf /tmp/work'

# 環境情報
ENV AMT_BASELIBS_DIR=/amt/baselibs
ENV AMT_PKGCONFIG_FFNK_DIR=/amt/ffmpeg_nekopanda/build/lib/pkgconfig
ENV AMT_PKGCONFIG_FF612_DIR=/amt/ffmpeg_612/build/lib/pkgconfig
