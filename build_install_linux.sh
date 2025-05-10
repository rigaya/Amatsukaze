#!/bin/sh

# 引数のチェック
if [ $# -lt 1 ]; then
    echo "Usage: $0 installdir [builddir] [--skip-plugins]"
    echo "  installdir: インストール先ディレクトリ"
    echo "  builddir: ビルド用ディレクトリ (省略時は 'build')"
    echo "  --skip-plugins: プラグインのインストールをスキップ"
    exit 1
fi

INSTALL_DIR="$1"
BUILD_DIR="${2:-build}"
SKIP_PLUGINS=0

# オプションの処理
for arg in "$@"; do
    case "$arg" in
        --skip-plugins)
            SKIP_PLUGINS=1
            ;;
    esac
done

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
  (git clone --depth 1 https://github.com/nekopanda/FFmpeg.git ffmpeg_nekopanda \
    && cd ffmpeg_nekopanda \
    && ./configure --prefix=`pwd`/build --enable-pic --disable-inline-asm --disable-xlib --disable-lzma --disable-bzlib --enable-gpl --enable-version3 --disable-programs --disable-doc --disable-network --disable-devices \
    && make -j$(nproc) \
    && make install) || exit 1
fi

# ffmpeg_nekopanda/buildを参照して、AmatsukazeCLIのビルドを行う
(meson setup --pkg-config-path `pwd`/ffmpeg_nekopanda/build/lib/pkgconfig ../.. && ninja) || exit 1
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
(meson setup --pkg-config-path `pwd`/ffmpeg-6.1.2/build/lib/pkgconfig ../.. && ninja) || exit 1
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
install -D -t "${INSTALL_DIR}/exe_files" ./scripts/AmatsukazeServer.sh
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


# プラグインのインストール関数
install_plugin() {
    local plugin_pattern="$1"
    local target_dir="${INSTALL_DIR}/exe_files/plugins64/"
    
    for plugin in $plugin_pattern; do
        if [ -e "$plugin" ]; then
            local plugin_name=$(basename "$plugin")
            local target_path="${target_dir}${plugin_name}"
            
            # 既存のファイルやリンクが存在しない場合のみリンクを作成
            if [ ! -e "$target_path" ]; then
                ln -sf "$plugin" "$target_dir"
                echo "プラグインへのリンクを作成しました: $plugin"
            else
                echo "既に存在するためスキップします: $plugin_name"
            fi
        fi
    done
}

# JLファイルのインストール
if [ ! -d "${INSTALL_DIR}/JL" ]; then
    (mkdir -p "${INSTALL_DIR}/JL" \
        && wget https://github.com/tobitti0/join_logo_scp/archive/refs/tags/Ver4.1.0_Linux.tar.gz \
        && tar -xf Ver4.1.0_Linux.tar.gz \
        && cp -r join_logo_scp-Ver4.1.0_Linux/JL/* "${INSTALL_DIR}/JL/" \
        && rm -rf join_logo_scp-Ver4.1.0_Linux Ver4.1.0_Linux.tar.gz) || exit 1
fi

# プラグインのインストール
if [ $SKIP_PLUGINS -eq 0 ]; then
    echo "プラグインへのリンクを作成します... -> ${INSTALL_DIR}/exe_files/plugins64/"

    # 各プラグインのインストール
    install_plugin "/usr/local/lib/avisynth/libyadifmod2*.so"
    install_plugin "/usr/local/lib/avisynth/libtivtc.so"
    install_plugin "/usr/local/lib/avisynth/libtdeint.so"
    install_plugin "/usr/local/lib/avisynth/KUtil.so"
    install_plugin "/usr/local/lib/avisynth/KFM.so"
    install_plugin "/usr/local/lib/avisynth/nnedi3.so"
    install_plugin "/usr/local/lib/avisynth/mt_masktools.so"
    install_plugin "/usr/local/lib/avisynth/KTGMC.so"
    install_plugin "/usr/local/lib/avisynth/AvsCUDA.so"
else
    echo "プラグインのインストールをスキップします"
fi

echo "インストールが完了しました"