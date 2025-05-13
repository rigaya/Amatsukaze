#!/bin/sh

# 引数のチェック
if [ $# -lt 1 ]; then
    echo "Usage: $0 installdir [builddir]"
    echo "  installdir: インストール先ディレクトリ"
    echo "  builddir: ビルド用ディレクトリ (省略時は 'build')"
    exit 1
fi

SCRIPT_DIR=`dirname $0`
SCRIPT_DIR=`cd ${SCRIPT_DIR} && pwd`

INSTALL_DIR="$1"
BUILD_DIR="${2:-build}"
# buildディレクトリがない場合は作成
if [ ! -d "${BUILD_DIR}" ]; then
    mkdir "${BUILD_DIR}"
fi

# buildディレクトリに移動
cd "${BUILD_DIR}"
# BUILD_DIR をフルパスに
BUILD_DIR=`pwd`

# libvplのビルド
if [ ! -d "libvpl-2.15.0" ]; then
    (wget https://github.com/intel/libvpl/archive/refs/tags/v2.15.0.tar.gz -O libvpl.tar.gz \
    && tar xf libvpl.tar.gz \
    && rm libvpl.tar.gz \
    && cd libvpl-2.15.0 \
    && cmake -G "Unix Makefiles" -B _build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${BUILD_DIR}/baselibs \
    && cd _build && make -j$(nproc) \
    && make install) || exit 1
    sed -i 's/-lvpl/-lvpl -lstdc++/g' ${BUILD_DIR}/baselibs/lib/pkgconfig/vpl.pc
fi

# media-sdkのビルド
if [ ! -d "MediaSDK-intel-mediasdk-22.5.4" ]; then
    (wget https://github.com/Intel-Media-SDK/MediaSDK/archive/refs/tags/intel-mediasdk-22.5.4.tar.gz -O intel-media-sdk.tar.gz \
    && tar xf intel-media-sdk.tar.gz \
    && rm intel-media-sdk.tar.gz \
    && cd MediaSDK-intel-mediasdk-22.5.4/api/mfx_dispatch/linux \
    && patch < ${SCRIPT_DIR}/mfx.diff \
    && cmake -G "Unix Makefiles" -B _build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${BUILD_DIR}/baselibs \
    && cd _build && make -j$(nproc) \
    && make install) || exit 1
fi

if [ ! -d "nv-codec-headers-12.2.72.0" ]; then
    (wget https://github.com/FFmpeg/nv-codec-headers/releases/download/n12.2.72.0/nv-codec-headers-12.2.72.0.tar.gz -O nv-codec-headers.tar.gz \
    && tar xf nv-codec-headers.tar.gz \
    && rm nv-codec-headers.tar.gz \
    && cd nv-codec-headers-12.2.72.0 \
    && make PREFIX=${BUILD_DIR}/baselibs install) || exit 1
fi

# ----- 地デジ/BS向け ffmpeg_nekopandaのAmatsukazeCLIのビルド -----
if [ ! -d "build_ffnk" ]; then
    mkdir build_ffnk
fi
cd build_ffnk
if [ ! -d "ffmpeg_nekopanda" ]; then
  (git clone --depth 1 -b amatsukaze https://github.com/nekopanda/FFmpeg.git ffmpeg_nekopanda \
    && cd ffmpeg_nekopanda \
    && wget https://github.com/FFmpeg/FFmpeg/commit/effadce6c756247ea8bae32dc13bb3e6f464f0eb.patch -O patch0.diff \
    && patch -p1 < patch0.diff \
    && CFLAGS="-w" PKG_CONFIG_PATH=${BUILD_DIR}/baselibs/lib/pkgconfig ./configure --prefix=`pwd`/build --enable-pic \
      --disable-iconv --disable-xlib --disable-lzma --disable-bzlib --disable-vaapi --enable-cuvid --enable-ffnvcodec --enable-libmfx \
      --enable-gpl --enable-version3 \
      --disable-autodetect --disable-doc --disable-network --disable-devices \
    && make -j$(nproc) \
    && make install) || exit 1
fi

# ffmpeg_nekopanda/buildを参照して、AmatsukazeCLIのビルドを行う
(meson setup --buildtype release --pkg-config-path `pwd`/ffmpeg_nekopanda/build/lib/pkgconfig ../.. && ninja) || exit 1
cd ..

# ----- BS4K向け ffmpeg_6.1.2ベースのAmatsukazeCLIのビルド -----
if [ ! -d "build_ff612" ]; then
    mkdir build_ff612
fi
cd build_ff612
if [ ! -d "ffmpeg-6.1.2" ]; then
  (wget https://www.ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz \
    && tar -xf ffmpeg-6.1.2.tar.xz \
    && cd ffmpeg-6.1.2 \
    && CFLAGS="-w" LDFLAGS="-lstdc++" PKG_CONFIG_PATH=${BUILD_DIR}/baselibs/lib/pkgconfig ./configure --prefix=`pwd`/build --enable-pic \
      --disable-iconv --disable-xlib --disable-lzma --disable-bzlib --disable-vaapi --enable-cuvid --enable-ffnvcodec --enable-libvpl \
      --enable-gpl --enable-version3 \
      --disable-autodetect --disable-doc --disable-network --disable-devices \
    && make -j$(nproc) \
    && make install) || exit 1
fi

# ffmpeg_6.1.2/buildを参照して、AmatsukazeCLIのビルドを行う
(meson setup --buildtype release --pkg-config-path `pwd`/ffmpeg-6.1.2/build/lib/pkgconfig ../.. && ninja) || exit 1
cp Amatsukaze/libAmatsukaze.so Amatsukaze/libAmatsukaze2.so
cd ..

# dotnet の AmatsukazeServer, AmatsukazeAddTask, AmatsukazeServerCLI のビルド
cd ..
(dotnet build AmatsukazeLinux.sln) || exit 1


# ----- インストール -----
# インストール先ディレクトリの作成
mkdir -p "${INSTALL_DIR}/avs"
mkdir -p "${INSTALL_DIR}/avscache"
mkdir -p "${INSTALL_DIR}/bat"
mkdir -p "${INSTALL_DIR}/drcs"
mkdir -p "${INSTALL_DIR}/exe_files"
mkdir -p "${INSTALL_DIR}/exe_files/plugins64"
mkdir -p "${INSTALL_DIR}/logo"
mkdir -p "${INSTALL_DIR}/profile"
mkdir -p "${INSTALL_DIR}/scripts"
touch "${INSTALL_DIR}/drcs/drcs_map.txt"

# 実行ファイルのインストール
echo "実行ファイルをインストールします..."
install -D -t "${INSTALL_DIR}" ./scripts/AmatsukazeServer.sh
install -D -t "${INSTALL_DIR}/exe_files" "${BUILD_DIR}/build_ffnk/AmatsukazeCLI/AmatsukazeCLI"
install -D -t "${INSTALL_DIR}/exe_files" "${BUILD_DIR}/build_ffnk/Amatsukaze/libAmatsukaze.so"
install -D -t "${INSTALL_DIR}/exe_files" "${BUILD_DIR}/build_ff612/Amatsukaze/libAmatsukaze2.so"


# .NET アプリケーションの公開
echo ".NET アプリケーションを公開します..."
if ! dotnet publish AmatsukazeLinux.sln -c Release -r linux-x64 --self-contained true -p:PublishSingleFile=true -o "${INSTALL_DIR}/exe_files"; then
    echo ".NET アプリケーションの公開に失敗しました"
    exit 1
fi

# ScriptCommand の展開
mkdir -p "${INSTALL_DIR}/exe_files/cmd"
for src_file in "${INSTALL_DIR}/exe_files/ScriptCommand."*; do
    cp "$src_file" "${INSTALL_DIR}/exe_files/cmd/"
done
for cmd in AddTag SetOutDir SetPriority GetOutFiles CancelItem; do
    cp "${INSTALL_DIR}/exe_files/ScriptCommand" "${INSTALL_DIR}/exe_files/cmd/${cmd}"
    echo "コマンドを作成しました: ${cmd} -> ${INSTALL_DIR}/exe_files/cmd/"
done

# defaultファイルのコピー
cp -r defaults/avs/*       "${INSTALL_DIR}/avs/"
cp -r defaults/bat_linux/* "${INSTALL_DIR}/bat/"
cp -r defaults/exe_files/* "${INSTALL_DIR}/exe_files/"
cp -r defaults/profile/*   "${INSTALL_DIR}/profile/"
cp -r scripts/*            "${INSTALL_DIR}/scripts/"

echo "インストールが完了しました"