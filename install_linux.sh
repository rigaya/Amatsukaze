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

# インストール先ディレクトリの作成
mkdir -p "${INSTALL_DIR}/exe_files"
mkdir -p "${INSTALL_DIR}/exe_files/plugins64"

# ビルドディレクトリの作成とビルド
echo "ビルドを開始します..."
if [ ! -d "${BUILD_DIR}" ]; then
    mkdir -p "${BUILD_DIR}"
fi
if ! (cd "${BUILD_DIR}" && meson setup .. && ninja); then
    echo "ビルドに失敗しました"
    exit 1
fi

# 実行ファイルのインストール
echo "実行ファイルをインストールします..."
install -D -t "${INSTALL_DIR}/exe_files" ./scripts/AmatsukazeServer.sh
install -D -t "${INSTALL_DIR}/exe_files" "${BUILD_DIR}/AmatsukazeCLI/AmatsukazeCLI"
install -D -t "${INSTALL_DIR}/exe_files" "${BUILD_DIR}/Amatsukaze/libAmatsukaze.so"

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

# プラグインのインストール
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

# .NET アプリケーションの公開
echo ".NET アプリケーションを公開します..."
if ! dotnet publish AmatsukazeLinux.sln -o "${INSTALL_DIR}/exe_files"; then
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

echo "インストールが完了しました"


