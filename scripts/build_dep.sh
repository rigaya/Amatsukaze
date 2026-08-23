#!/bin/sh
set -e

# Usage: build_dep.sh [DEST_ROOT]
#  - DEST_ROOT=/amt のときは /amt 配下にプレインストール
#  - それ以外（未指定含む）はカレントの既定構成（build_*配下）にインストール

DEST_ROOT="${1:-}"
BUILD_DIR="$(pwd)"

SCRIPT_DIR=`dirname $0`
SCRIPT_DIR=`cd ${SCRIPT_DIR} && pwd`

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

# libjpeg-turbo (静的ビルド)
if [ ! -d "${BUILD_DIR}/libjpeg-turbo-3.1.0" ]; then
  echo "libjpeg-turbo のビルドを行います。"
  (
    wget https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.0/libjpeg-turbo-3.1.0.tar.gz -O libjpeg-turbo.tar.gz \
    && tar xf libjpeg-turbo.tar.gz \
    && rm libjpeg-turbo.tar.gz \
    && cd libjpeg-turbo-3.1.0 \
    && cmake -G "Unix Makefiles" -B _build \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${BASELIBS_DIR}" \
      -DENABLE_SHARED=OFF \
      -DENABLE_STATIC=ON \
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

if [ ! -f "${FNNK_PREFIX}/lib/pkgconfig/libavcodec.pc" ]; then
  echo "ffmpeg (地デジ/BS向け) のビルドを行います。"
  if [ ! -d "${SRC_DIR}" ]; then
    (
      git clone --depth 1 -b amatsukaze https://github.com/nekopanda/FFmpeg.git "${SRC_DIR}" \
      && cd "${SRC_DIR}" \
      && wget https://github.com/FFmpeg/FFmpeg/commit/effadce6c756247ea8bae32dc13bb3e6f464f0eb.patch -O patch0.diff \
      && patch -p1 < patch0.diff
    )
  fi
  # この旧FFmpegはCUDAをx86限定で無効化するため、Linux ARM64も許可する
  (
    cd "${SRC_DIR}" \
    && sed -i 's/ffnvcodec_deps_any="[^"]*"/ffnvcodec_deps_any="libdl LoadLibrary"/' configure \
    && sed -i '/^if enabled x86; then$/ { N; /    case \$target_os in/ s/enabled x86/enabled_any x86 aarch64/; }' configure \
    && CFLAGS="-w" PKG_CONFIG_PATH="${BASELIBS_DIR}/lib/pkgconfig" ./configure --prefix="${FNNK_PREFIX}" --enable-pic \
      --disable-iconv --disable-xlib --disable-lzma --disable-bzlib --disable-vaapi --enable-cuvid --enable-ffnvcodec \
      --enable-gpl --enable-version3 \
      --disable-doc --disable-network --disable-devices \
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

if [ ! -f "${FF612_PREFIX}/lib/pkgconfig/libavcodec.pc" ]; then
  echo "ffmpeg (BS4K向け) のビルドを行います。"
  if [ ! -d "${FF612_SRC}" ]; then
    (
      mkdir -p "${FF612_SRC_PARENT}" \
      && cd "${FF612_SRC_PARENT}" \
      && wget https://www.ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz \
      && tar -xf ffmpeg-6.1.2.tar.xz
    )
  fi
  (
    cd "${FF612_SRC}" \
    && CFLAGS="-w" LDFLAGS="-lstdc++" PKG_CONFIG_PATH="${BASELIBS_DIR}/lib/pkgconfig" ./configure --prefix="${FF612_PREFIX}" --enable-pic \
      --disable-iconv --disable-xlib --disable-lzma --disable-bzlib --disable-vaapi --enable-cuvid --enable-ffnvcodec \
      --enable-gpl --enable-version3 \
      --disable-doc --disable-network --disable-devices \
    && make -j"$(nproc)" \
    && make install
  )
fi

echo "依存ビルドが完了しました。出力先: ${DEST_ROOT:-${BUILD_DIR}}"
