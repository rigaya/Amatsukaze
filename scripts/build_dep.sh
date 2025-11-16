#!/bin/sh
set -e

# Usage: build_dep.sh [DEST_ROOT]
#  - DEST_ROOT=/amt のときは /amt 配下にプレインストール
#  - それ以外（未指定含む）はカレントの既定構成（build_*配下）にインストール

DEST_ROOT="${1:-}"
BUILD_DIR="$(pwd)"

is_amt_dest=0
if [ -n "${DEST_ROOT}" ] && [ "${DEST_ROOT}" = "/amt" ]; then
  is_amt_dest=1
fi

# 共通: baselibs 出力先
if [ ${is_amt_dest} -eq 1 ]; then
  BASELIBS_DIR="${DEST_ROOT}/baselibs"
else
  BASELIBS_DIR="${BUILD_DIR}/baselibs"
fi

mkdir -p "${BASELIBS_DIR}"

# libvpl
if [ ! -d "${BUILD_DIR}/libvpl-2.15.0" ]; then
  echo "libvpl のビルドを行います。"
  (
    wget https://github.com/intel/libvpl/archive/refs/tags/v2.15.0.tar.gz -O libvpl.tar.gz \
    && tar xf libvpl.tar.gz \
    && rm libvpl.tar.gz \
    && cd libvpl-2.15.0 \
    && cmake -G "Unix Makefiles" -B _build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${BASELIBS_DIR}" \
    && cd _build && make -j"$(nproc)" \
    && make install
  )
  # g++ リンクを要求するパッケージのために -lstdc++ を追記
  if [ -f "${BASELIBS_DIR}/lib/pkgconfig/vpl.pc" ]; then
    sed -i 's/-lvpl/-lvpl -lstdc++/g' "${BASELIBS_DIR}/lib/pkgconfig/vpl.pc"
  fi
fi

# MediaSDK (dispatch)
if [ ! -d "${BUILD_DIR}/MediaSDK-intel-mediasdk-22.5.4" ]; then
  echo "Intel MediaSDK のビルドを行います。"
  (
    wget https://github.com/Intel-Media-SDK/MediaSDK/archive/refs/tags/intel-mediasdk-22.5.4.tar.gz -O intel-media-sdk.tar.gz \
    && tar xf intel-media-sdk.tar.gz \
    && rm intel-media-sdk.tar.gz \
    && cd MediaSDK-intel-mediasdk-22.5.4/api/mfx_dispatch/linux \
    && patch < "${BUILD_DIR}/scripts/mfx.diff" \
    && cmake -G "Unix Makefiles" -B _build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${BASELIBS_DIR}" \
    && cd _build && make -j"$(nproc)" \
    && make install
  )
fi

# nv-codec-headers
if [ ! -d "${BUILD_DIR}/nv-codec-headers-12.2.72.0" ]; then
  echo "nv-codec-headers のビルドを行います。"
  (
    wget https://github.com/FFmpeg/nv-codec-headers/releases/download/n12.2.72.0/nv-codec-headers-12.2.72.0.tar.gz -O nv-codec-headers.tar.gz \
    && tar xf nv-codec-headers.tar.gz \
    && rm nv-codec-headers.tar.gz \
    && cd nv-codec-headers-12.2.72.0 \
    && make PREFIX="${BASELIBS_DIR}" install
  )
fi

# ffmpeg_nekopanda (地デジ/BS向け)
if [ ${is_amt_dest} -eq 1 ]; then
  FNNK_PREFIX="${DEST_ROOT}/ffmpeg_nekopanda/build"
  SRC_DIR="${BUILD_DIR}/_deps_ffnk_src"
else
  # 既存の構成と互換なレイアウト
  mkdir -p "${BUILD_DIR}/build_ffnk"
  SRC_DIR="${BUILD_DIR}/build_ffnk/ffmpeg_nekopanda"
  FNNK_PREFIX="${SRC_DIR}/build"
fi

if [ ! -d "${SRC_DIR}" ]; then
  echo "ffmpeg (地デジ/BS向け) のビルドを行います。"
  (
    git clone --depth 1 -b amatsukaze https://github.com/nekopanda/FFmpeg.git "${SRC_DIR}" \
    && cd "${SRC_DIR}" \
    && wget https://github.com/FFmpeg/FFmpeg/commit/effadce6c756247ea8bae32dc13bb3e6f464f0eb.patch -O patch0.diff \
    && patch -p1 < patch0.diff \
    && CFLAGS="-w" PKG_CONFIG_PATH="${BASELIBS_DIR}/lib/pkgconfig" ./configure --prefix="${FNNK_PREFIX}" --enable-pic \
      --disable-iconv --disable-xlib --disable-lzma --disable-bzlib --disable-vaapi --enable-cuvid --enable-ffnvcodec --enable-libmfx \
      --enable-gpl --enable-version3 \
      --disable-autodetect --disable-doc --disable-network --disable-devices \
    && make -j"$(nproc)" \
    && make install
  )
fi

# ffmpeg-6.1.2 (BS4K向け)
if [ ${is_amt_dest} -eq 1 ]; then
  FF612_PREFIX="${DEST_ROOT}/ffmpeg_612/build"
  FF612_SRC_PARENT="${BUILD_DIR}/_deps_ff612_src"
  FF612_SRC="${FF612_SRC_PARENT}/ffmpeg-6.1.2"
else
  mkdir -p "${BUILD_DIR}/build_ff612"
  FF612_SRC_PARENT="${BUILD_DIR}/build_ff612"
  FF612_SRC="${FF612_SRC_PARENT}/ffmpeg-6.1.2"
  FF612_PREFIX="${FF612_SRC}/build"
fi

if [ ! -d "${FF612_SRC}" ]; then
  echo "ffmpeg (BS4K向け) のビルドを行います。"
  (
    mkdir -p "${FF612_SRC_PARENT}" \
    && cd "${FF612_SRC_PARENT}" \
    && wget https://www.ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz \
    && tar -xf ffmpeg-6.1.2.tar.xz \
    && cd ffmpeg-6.1.2 \
    && CFLAGS="-w" LDFLAGS="-lstdc++" PKG_CONFIG_PATH="${BASELIBS_DIR}/lib/pkgconfig" ./configure --prefix="${FF612_PREFIX}" --enable-pic \
      --disable-iconv --disable-xlib --disable-lzma --disable-bzlib --disable-vaapi --enable-cuvid --enable-ffnvcodec --enable-libvpl \
      --enable-gpl --enable-version3 \
      --disable-autodetect --disable-doc --disable-network --disable-devices \
    && make -j"$(nproc)" \
    && make install
  )
fi

echo "依存ビルドが完了しました。出力先: ${DEST_ROOT:-${BUILD_DIR}}"


