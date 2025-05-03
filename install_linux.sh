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
cp "${BUILD_DIR}/AmatsukazeCLI/AmatsukazeCLI" "${INSTALL_DIR}/exe_files/"
cp "${BUILD_DIR}/Amatsukaze/libAmatsukaze.so" "${INSTALL_DIR}/exe_files/"

# .NET アプリケーションの公開
echo ".NET アプリケーションを公開します..."
if ! dotnet publish AmatsukazeLinux.sln -o "${INSTALL_DIR}/exe_files"; then
    echo ".NET アプリケーションの公開に失敗しました"
    exit 1
fi

echo "インストールが完了しました"


