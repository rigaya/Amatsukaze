#!/bin/sh

# 引数のチェック
if [ "$1" = "-h" ]; then
    echo "Usage: $0 [installdir]"
    echo "  installdir: インストール先ディレクトリ（省略時はカレントディレクトリ）"
    exit 0
fi

if [ $# -lt 1 ]; then
    INSTALL_DIR="."
else
    INSTALL_DIR="$1"
fi

SKIP_PLUGINS=0

# プラグインのインストール関数
install_plugin() {
    local plugin_pattern="$1"
    local search_dir="/usr/local/lib/avisynth/"
    local target_dir="${INSTALL_DIR}/exe_files/plugins64/"

    # すでにtarget_dirにファイルが存在する場合はスキップ
    if [ -e "${target_dir}${plugin_pattern}" ]; then
        echo "既に存在するためスキップします"
        return 0
    fi

    # プラグインの存在確認
    if ! ls ${search_dir}${plugin_pattern} >/dev/null 2>&1; then
        echo "プラグインが見つかりません: ${search_dir}${plugin_pattern}"
        return 1
    fi

    find ${search_dir} -name "${plugin_pattern}" | while read plugin; do
        ln -sf "$plugin" "$target_dir"
        echo "プラグインへのリンクを作成しました: $plugin"
    done

    return 0
}

# プラグインのインストール
if [ $SKIP_PLUGINS -eq 0 ]; then
    echo "プラグインへのリンクを作成します... -> ${INSTALL_DIR}/exe_files/plugins64/"

    # 各プラグインのインストール
    install_plugin "KUtil.so"
    install_plugin "KFM.so"
    install_plugin "nnedi3.so"
    install_plugin "mt_masktools.so"
    install_plugin "KTGMC.so"
    install_plugin "AvsCUDA.so"
    install_plugin "libmasktools2.so"
    install_plugin "libyadifmod2*.so"
    install_plugin "libtivtc.so"
    install_plugin "libtdeint.so"
    install_plugin "librgtools.so"
else
    echo "プラグインのインストールをスキップします"
fi

# スクリプトへの実行権限付与
find ${INSTALL_DIR}/bat -type f -name "*.sh" | xargs chmod u+x

echo "インストールが完了しました"