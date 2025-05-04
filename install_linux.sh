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

# プラグインのインストール
echo "プラグインへのリンクを作成します... -> ${INSTALL_DIR}/exe_files/plugins64/"
# libyadifmod2*.so をインストール
for plugin in /usr/local/lib/avisynth/libyadifmod2*.so; do
    if [ -e "$plugin" ]; then
        ln -sf "$plugin" "${INSTALL_DIR}/exe_files/plugins64/"
        echo "プラグインへのリンクを作成しました: $plugin"
    fi
done

# libtivtc.so をインストール
for plugin in /usr/local/lib/avisynth/libtivtc.so; do
    if [ -e "$plugin" ]; then
        ln -sf "$plugin" "${INSTALL_DIR}/exe_files/plugins64/"
        echo "プラグインへのリンクを作成しました: $plugin"
    fi
done

# libtdeint.so をインストール
for plugin in /usr/local/lib/avisynth/libtdeint.so; do
    if [ -e "$plugin" ]; then
        ln -sf "$plugin" "${INSTALL_DIR}/exe_files/plugins64/"
        echo "プラグインへのリンクを作成しました: $plugin"
    fi
done

# .NET アプリケーションの公開
echo ".NET アプリケーションを公開します..."
if ! dotnet publish AmatsukazeLinux.sln -o "${INSTALL_DIR}/exe_files"; then
    echo ".NET アプリケーションの公開に失敗しました"
    exit 1
fi

# ScriptCommand の展開
mkdir -p "${INSTALL_DIR}/exe_files/cmd"
for cmd in AddTag SetOutDir SetPriority GetOutFiles CancelItem; do
    # ScriptCommandで始まるすべてのファイルに対して処理を行う
    for src_file in "${INSTALL_DIR}/exe_files/ScriptCommand"*; do
        if [ -f "$src_file" ]; then
            # ファイル名からScriptCommand部分を除去し、新しいコマンド名に置換
            dst_file="${INSTALL_DIR}/exe_files/cmd/${cmd}${src_file#${INSTALL_DIR}/exe_files/ScriptCommand}"
            cp "$src_file" "$dst_file"
        fi
        # ファイル名がScriptCommandそのもののときだけメッセージを表示
        if [ "$src_file" = "${INSTALL_DIR}/exe_files/ScriptCommand" ]; then
            echo "コマンドを作成しました: ${cmd} -> ${INSTALL_DIR}/exe_files/cmd/"
        fi
    done
done

echo "インストールが完了しました"


