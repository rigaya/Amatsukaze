#!/bin/sh

# 引数のチェック
if [ $# -lt 1 ]; then
    echo "Usage: $0 installdir [builddir]"
    echo "  installdir: インストール先ディレクトリ"
    echo "  builddir: ビルド用ディレクトリ (省略時は 'build')"
    exit 1
fi

INSTALL_DIR="$1"
BUILD_DIR="${2:-build}"

# buildディレクトリがない場合は作成
if [ ! -d "${BUILD_DIR}" ]; then
    mkdir "${BUILD_DIR}"
fi

# buildディレクトリに移動
cd "${BUILD_DIR}"
if [ ! -d "build_ffnk" ]; then
    mkdir build_ffnk
fi

# ----- 地デジ/BS向け ffmpeg_nekopandaのAmatsukazeCLIのビルド -----
cd build_ffnk
if [ ! -d "ffmpeg_nekopanda" ]; then
  (git clone --depth 1 -b amatsukaze https://github.com/nekopanda/FFmpeg.git ffmpeg_nekopanda \
    && cd ffmpeg_nekopanda \
    && wget https://github.com/FFmpeg/FFmpeg/commit/effadce6c756247ea8bae32dc13bb3e6f464f0eb.patch -O patch0.diff \
    && patch -p1 < patch0.diff \
    && ./configure --prefix=`pwd`/build --enable-pic --extra-cflags="-Wno-attributes" --as=yasm --disable-xlib --disable-lzma --disable-bzlib --enable-gpl --enable-version3 --disable-programs --disable-doc --disable-network --disable-devices \
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
    && ./configure --prefix=`pwd`/build --enable-pic --disable-xlib --disable-lzma --disable-bzlib --enable-gpl --enable-version3 --disable-programs --disable-doc --disable-network --disable-devices \
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
touch "${INSTALL_DIR}/drcs/drcs_map.txt"

# 実行ファイルのインストール
echo "実行ファイルをインストールします..."
install -D -t "${INSTALL_DIR}" ./scripts/AmatsukazeServer.sh
install -D -t "${INSTALL_DIR}/exe_files" "${BUILD_DIR}/build_ffnk/AmatsukazeCLI/AmatsukazeCLI"
install -D -t "${INSTALL_DIR}/exe_files" "${BUILD_DIR}/build_ffnk/Amatsukaze/libAmatsukaze.so"
install -D -t "${INSTALL_DIR}/exe_files" "${BUILD_DIR}/build_ff612/Amatsukaze/libAmatsukaze2.so"


# .NET アプリケーションの公開
echo ".NET アプリケーションを公開します..."
if ! dotnet publish AmatsukazeLinux.sln -c Release -r linux-x64 --self-contained true -o "${INSTALL_DIR}/exe_files"; then
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

echo "インストールが完了しました"