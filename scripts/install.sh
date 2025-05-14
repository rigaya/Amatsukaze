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

# JLファイルのインストール
if [ ! -d "${INSTALL_DIR}/JL" ]; then
    echo "JLファイルのインストールを開始します..."
    (mkdir -p "${INSTALL_DIR}/JL" \
        && wget https://github.com/tobitti0/join_logo_scp/archive/refs/tags/Ver4.1.0_Linux.tar.gz \
        && tar -xf Ver4.1.0_Linux.tar.gz \
        && cp -r join_logo_scp-Ver4.1.0_Linux/JL/* "${INSTALL_DIR}/JL/" \
        && rm -rf join_logo_scp-Ver4.1.0_Linux Ver4.1.0_Linux.tar.gz) || exit 1
fi

# プラグインのインストール関数
install_plugin() {
    local plugin_pattern="$1"
    local search_dir="/usr/local/lib/avisynth/"
    local target_dir="${INSTALL_DIR}/exe_files/plugins64/"

    # すでにtarget_dirにファイルが存在する場合はスキップ
    if [ -e "${target_dir}${plugin_pattern}" ]; then
        echo "既に存在するためスキップします"
        exit 0
    fi

    # ファイルが存在しない場合はエラー
    if [ ! -e "${search_dir}${plugin_pattern}" ]; then
        echo "プラグインが見つかりません: ${search_dir}${plugin_pattern}"
        exit 1
    fi

    for plugin in ${search_dir}${plugin_pattern}; do
        if [ -e "$plugin" ]; then
            ln -sf "$plugin" "$target_dir"
            echo "プラグインへのリンクを作成しました: $plugin"
        fi
    done
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
find ${INSTALL_DIR}/bat -name "*.sh" | xargs chmod u+x

echo "インストールが完了しました"