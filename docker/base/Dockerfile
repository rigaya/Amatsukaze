# syntax=docker/dockerfile:1.6
ARG UBUNTU_VERSION=22.04
FROM ubuntu:${UBUNTU_VERSION}

ARG DEBIAN_FRONTEND=noninteractive
ARG UBUNTU_NAME=jammy

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
      cmake \
      nasm \
      yasm \
      meson \
      ninja-build \
      pkg-config \
      checkinstall \
      autoconf \
      automake \
      libtool \
      libssl-dev \
      libz-dev \
      build-essential \
      locales \
    && locale-gen ja_JP.UTF-8 \
    && locale-gen en_US.UTF-8 \
    && update-locale LANG=ja_JP.UTF-8 \
    && add-apt-repository ppa:dotnet/backports \
    && apt update \
    && apt-get install -y dotnet-sdk-10.0 \
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


