#!/bin/sh

# 引数のチェック
if [ $# -lt 1 ]; then
    echo "Usage: $0 installdir [builddir] [--debug]"
    echo "  installdir: インストール先ディレクトリ"
    echo "  builddir: ビルド用ディレクトリ (省略時は 'build')"
    echo "  --debug: デバッグビルドを実行 (省略時はリリースビルド)"
    exit 1
fi

SCRIPT_DIR=`dirname $0`
SCRIPT_DIR=`cd ${SCRIPT_DIR} && pwd`

INSTALL_DIR="$1"
BUILD_DIR="${2:-build}"

# .NET 10 SDK 必須チェック
if ! command -v dotnet >/dev/null 2>&1; then
    echo "dotnet コマンドが見つかりません。.NET 10 SDK をインストールしてください。"
    exit 1
fi
if ! dotnet --list-sdks | awk '{print $1}' | grep -Eq '^10\.'; then
    echo ".NET 10 SDK が見つかりません。dotnet --list-sdks を確認してください。"
    exit 1
fi

# デバッグビルドのオプションをチェック
DEBUG_BUILD=false
if [ "$2" = "--debug" ] || [ "$3" = "--debug" ]; then
    DEBUG_BUILD=true
    echo "デバッグビルドモードで実行します"
    # builddirが--debugの場合はデフォルト値を使用
    if [ "$2" = "--debug" ]; then
        BUILD_DIR="build"
    fi
else
    echo "リリースビルドモードで実行します"
fi
# 依存のみビルドオプション
DEPS_ONLY=false
if [ "$2" = "--deps-only" ] || [ "$3" = "--deps-only" ]; then
    DEPS_ONLY=true
    # builddirが--deps-onlyの場合はデフォルト値を使用
    if [ "$2" = "--deps-only" ]; then
        BUILD_DIR="build"
    fi
fi
# buildディレクトリがない場合は作成
if [ ! -d "${BUILD_DIR}" ]; then
    mkdir "${BUILD_DIR}"
fi

# buildディレクトリに移動
cd "${BUILD_DIR}" || exit 1
# BUILD_DIR をフルパスに
BUILD_DIR=`pwd`

# 依存のみビルドして終了
if [ "${DEPS_ONLY}" = "true" ]; then
    echo "依存のみをビルドします (DEST=${INSTALL_DIR})"
    "${SCRIPT_DIR}/build_dep.sh" "${INSTALL_DIR}"
    exit 0
fi

# /amt の事前ビルド検出とリンク設定
AMT_BASELIBS_DIR=${AMT_BASELIBS_DIR:-/amt/baselibs}
AMT_PKGCONFIG_FFNK_DIR=${AMT_PKGCONFIG_FFNK_DIR:-/amt/ffmpeg_nekopanda/build/lib/pkgconfig}
AMT_PKGCONFIG_FF612_DIR=${AMT_PKGCONFIG_FF612_DIR:-/amt/ffmpeg_612/build/lib/pkgconfig}
USE_PREBUILT_BASELIBS=0
USE_PREBUILT_FFNK=0
USE_PREBUILT_FF612=0
if [ -d "${AMT_BASELIBS_DIR}" ]; then
    ln -sfn "${AMT_BASELIBS_DIR}" "${BUILD_DIR}/baselibs"
    export PKG_CONFIG_PATH="${BUILD_DIR}/baselibs/lib/pkgconfig:${PKG_CONFIG_PATH}"
    USE_PREBUILT_BASELIBS=1
fi
if [ -d "${AMT_PKGCONFIG_FFNK_DIR}" ]; then
    USE_PREBUILT_FFNK=1
fi
if [ -d "${AMT_PKGCONFIG_FF612_DIR}" ]; then
    USE_PREBUILT_FF612=1
fi

# フォールバック検出（ENV未設定でも /amt がある場合を考慮）
if [ "${USE_PREBUILT_BASELIBS}" != "1" ] && [ -d "/amt/baselibs/lib/pkgconfig" ]; then
    ln -sfn "/amt/baselibs" "${BUILD_DIR}/baselibs"
    export PKG_CONFIG_PATH="${BUILD_DIR}/baselibs/lib/pkgconfig:${PKG_CONFIG_PATH}"
    USE_PREBUILT_BASELIBS=1
fi
if [ "${USE_PREBUILT_FFNK}" != "1" ] && [ -d "/amt/ffmpeg_nekopanda/build/lib/pkgconfig" ]; then
    AMT_PKGCONFIG_FFNK_DIR="/amt/ffmpeg_nekopanda/build/lib/pkgconfig"
    USE_PREBUILT_FFNK=1
fi
if [ "${USE_PREBUILT_FF612}" != "1" ] && [ -d "/amt/ffmpeg_612/build/lib/pkgconfig" ]; then
    AMT_PKGCONFIG_FF612_DIR="/amt/ffmpeg_612/build/lib/pkgconfig"
    USE_PREBUILT_FF612=1
fi

echo "${BUILD_DIR} にインストールを行います。"

# libvplのビルド
if [ "${USE_PREBUILT_BASELIBS}" != "1" ] && [ ! -d "libvpl-2.15.0" ]; then
    echo "libvpl のビルドを行います。"
    (wget https://github.com/intel/libvpl/archive/refs/tags/v2.15.0.tar.gz -O libvpl.tar.gz \
    && tar xf libvpl.tar.gz \
    && rm libvpl.tar.gz \
    && cd libvpl-2.15.0 \
    && cmake -G "Unix Makefiles" -B _build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${BUILD_DIR}/baselibs \
    && cd _build && make -j$(nproc) \
    && make install) || exit 1
    sed -i 's/-lvpl/-lvpl -lstdc++/g' ${BUILD_DIR}/baselibs/lib/pkgconfig/vpl.pc
fi
if [ "${USE_PREBUILT_BASELIBS}" = "1" ]; then
    echo "prebuilt baselibs を使用します。libvpl のビルドをスキップします。"
fi

# media-sdkのビルド
if [ "${USE_PREBUILT_BASELIBS}" != "1" ] && [ ! -d "MediaSDK-intel-mediasdk-22.5.4" ]; then
    echo "Intel MediaSDK のビルドを行います。"
    (wget https://github.com/Intel-Media-SDK/MediaSDK/archive/refs/tags/intel-mediasdk-22.5.4.tar.gz -O intel-media-sdk.tar.gz \
    && tar xf intel-media-sdk.tar.gz \
    && rm intel-media-sdk.tar.gz \
    && cd MediaSDK-intel-mediasdk-22.5.4/api/mfx_dispatch/linux \
    && patch < ${SCRIPT_DIR}/mfx.diff \
    && cmake -G "Unix Makefiles" -B _build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${BUILD_DIR}/baselibs \
    && cd _build && make -j$(nproc) \
    && make install) || exit 1
fi
if [ "${USE_PREBUILT_BASELIBS}" = "1" ]; then
    echo "prebuilt baselibs を使用します。MediaSDK のビルドをスキップします。"
fi

if [ ! -d "nv-codec-headers-12.2.72.0" ]; then
    if [ "${USE_PREBUILT_BASELIBS}" != "1" ]; then
        echo "nv-codec-headers のビルドを行います。"
        (wget https://github.com/FFmpeg/nv-codec-headers/releases/download/n12.2.72.0/nv-codec-headers-12.2.72.0.tar.gz -O nv-codec-headers.tar.gz \
        && tar xf nv-codec-headers.tar.gz \
        && rm nv-codec-headers.tar.gz \
        && cd nv-codec-headers-12.2.72.0 \
        && make PREFIX=${BUILD_DIR}/baselibs install) || exit 1
    else
        echo "prebuilt baselibs を使用します。nv-codec-headers のビルドをスキップします。"
    fi
fi

# ----- 地デジ/BS向け ffmpeg_nekopandaのAmatsukazeCLIのビルド -----
if [ ! -d "build_ffnk" ]; then
    mkdir build_ffnk
fi
cd build_ffnk
if [ ! -d "ffmpeg_nekopanda" ] && [ "${USE_PREBUILT_FFNK}" != "1" ]; then
    echo "ffmpeg (地デジ/BS向け) のビルドを行います。"
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
echo "AmatsukazeCLI (地デジ/BS向け) のビルドを行います。"
FFNK_PKGCFG_PATH="`pwd`/ffmpeg_nekopanda/build/lib/pkgconfig"
if [ "${USE_PREBUILT_FFNK}" = "1" ]; then
    FFNK_PKGCFG_PATH="${AMT_PKGCONFIG_FFNK_DIR}"
fi
(meson setup --buildtype release --pkg-config-path "${FFNK_PKGCFG_PATH}" ../.. && ninja) || exit 1
cd ..

# ----- BS4K向け ffmpeg_6.1.2ベースのAmatsukazeCLIのビルド -----
if [ ! -d "build_ff612" ]; then
    mkdir build_ff612
fi
cd build_ff612
if [ ! -d "ffmpeg-6.1.2" ] && [ "${USE_PREBUILT_FF612}" != "1" ]; then
    echo "ffmpeg (BS4K向け) のビルドを行います。"
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
echo "AmatsukazeCLI (BS4K向け) のビルドを行います。"
FF612_PKGCFG_PATH="`pwd`/ffmpeg-6.1.2/build/lib/pkgconfig"
if [ "${USE_PREBUILT_FF612}" = "1" ]; then
  FF612_PKGCFG_PATH="${AMT_PKGCONFIG_FF612_DIR}"
fi
(meson setup --buildtype release --pkg-config-path "${FF612_PKGCFG_PATH}" ../.. && ninja) || exit 1
cp Amatsukaze/libAmatsukaze.so Amatsukaze/libAmatsukaze2.so
cd ..

# dotnet の AmatsukazeServer, AmatsukazeAddTask, AmatsukazeServerCLI のビルド
if [ "$DEBUG_BUILD" = true ]; then
    echo "AmatsukazeServer, AmatsukazeAddTask, AmatsukazeServerCLI のデバッグビルドを行います。"
    cd ..
    (dotnet build AmatsukazeLinux.sln -c Debug) || exit 1
else
    echo "AmatsukazeServer, AmatsukazeAddTask, AmatsukazeServerCLI のリリースビルドを行います。"
    cd ..
    (dotnet build AmatsukazeLinux.sln -c Release) || exit 1
fi


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
if [ "$DEBUG_BUILD" = true ]; then
    echo ".NET アプリケーションをデバッグモードで公開します..."
    DOTNET_PUBLISH_CONFIG=Debug
else
    echo ".NET アプリケーションをリリースモードで公開します..."
    DOTNET_PUBLISH_CONFIG=Release
fi

# ソリューション全体の publish だと WebUI が PublishSingleFile に非対応のため失敗するため、
# 実行ファイルが必要なプロジェクトのみ publish する
DOTNET_PUBLISH_PROJECTS="
AmatsukazeServerCLI/AmatsukazeServerCLI.csproj
AmatsukazeAddTask/AmatsukazeAddTask.csproj
ScriptCommand/ScriptCommand.csproj
"
for project in ${DOTNET_PUBLISH_PROJECTS}; do
    if ! dotnet publish "${project}" -c "${DOTNET_PUBLISH_CONFIG}" -r linux-x64 --self-contained true -p:PublishSingleFile=true -o "${INSTALL_DIR}/exe_files"; then
        echo ".NET アプリケーションの公開に失敗しました (${project})"
        exit 1
    fi
done

# WebUI 静的ファイルの公開（AmatsukazeServer.csproj の AfterPublish を利用）
WEBUI_PUBLISH_DIR="${BUILD_DIR}/webui_publish"
echo "WebUI (static) を公開します..."
if ! dotnet publish "AmatsukazeServer/AmatsukazeServer.csproj" -c "${DOTNET_PUBLISH_CONFIG}" -r linux-x64 --self-contained false -p:PublishSingleFile=false -o "${WEBUI_PUBLISH_DIR}"; then
    echo "WebUI の公開に失敗しました"
    exit 1
fi
if [ -d "${WEBUI_PUBLISH_DIR}/wwwroot" ]; then
    rm -rf "${INSTALL_DIR}/exe_files/wwwroot"
    cp -r "${WEBUI_PUBLISH_DIR}/wwwroot" "${INSTALL_DIR}/exe_files/wwwroot"
fi
# defaultファイルのコピー
cp -r defaults/avs/*       "${INSTALL_DIR}/avs/"
cp -r defaults/bat_linux/* "${INSTALL_DIR}/bat/"
cp -r defaults/exe_files/* "${INSTALL_DIR}/exe_files/"
cp -r defaults/profile/*   "${INSTALL_DIR}/profile/"
cp -r scripts/*            "${INSTALL_DIR}/scripts/"

# ラッパースクリプトに実行権限を付与
if [ -d "${INSTALL_DIR}/exe_files/cmd" ]; then
  chmod +x "${INSTALL_DIR}/exe_files/cmd"/* || true
fi

# JLファイルのインストール
if [ ! -d "${INSTALL_DIR}/JL" ]; then
    echo "JLファイルのインストールを開始します..."
    (mkdir -p "${INSTALL_DIR}/JL" \
        && wget https://github.com/tobitti0/join_logo_scp/archive/refs/tags/Ver4.1.0_Linux.tar.gz \
        && tar -xf Ver4.1.0_Linux.tar.gz \
        && cp -r join_logo_scp-Ver4.1.0_Linux/JL/* "${INSTALL_DIR}/JL/" \
        && rm -rf join_logo_scp-Ver4.1.0_Linux Ver4.1.0_Linux.tar.gz) || exit 1
fi

echo "インストールが完了しました (WebUI は REST ポート+1 で公開されます)"
