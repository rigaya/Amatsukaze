#!/bin/bash

# Amatsukaze Docker Compose セットアップスクリプト
# 必要なディレクトリの構成と設定ファイルの準備を行います

set -e

# ヘルプメッセージ
show_help() {
    cat << EOF
Amatsukaze Docker Compose セットアップスクリプト

使用方法:
    $0 [オプション]

オプション:
    -h, --help             このヘルプを表示

このスクリプトは以下の処理を行います:
    1. 必要なディレクトリの作成
    2. 設定ファイルのコピー
    3. compose.ymlファイルの準備

セットアップ完了後は以下のコマンドを実行してください:
    export RUN_UID=\$(id -u)
    export RUN_GID=\$(id -g)
    docker compose up -d

例:
    # セットアップの実行
    $0

    # セットアップ完了後の起動
    export RUN_UID=\$(id -u)
    export RUN_GID=\$(id -g)
    docker compose up -d
EOF
}

# 引数の解析
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "不明なオプション: $1"
            show_help
            exit 1
            ;;
    esac
done

# ディレクトリセットアップ関数
setup_directories() {
    echo "=== Amatsukaze Docker Compose セットアップ ==="
    echo ""
    
    # setup.shのディレクトリを取得
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    DEFAULTS_DIR="$SCRIPT_DIR/../defaults"
    CURRENT_DIR="$(pwd)"
    
    # ディレクトリのコピー
    echo "--- ディレクトリのコピー ---"
    
    # avsディレクトリのコピー
    if [[ ! -d "$CURRENT_DIR/avs" ]]; then
        if [[ -d "$DEFAULTS_DIR/avs" ]]; then
            cp -r "$DEFAULTS_DIR/avs" "$CURRENT_DIR/"
            echo "✅ avsディレクトリをコピーしました"
        else
            echo "⚠️  $DEFAULTS_DIR/avs が見つかりません"
        fi
    else
        echo "ℹ️  avsディレクトリは既に存在します"
    fi
    
    # batディレクトリのコピー（bat_linuxから）
    if [[ ! -d "$CURRENT_DIR/bat" ]]; then
        if [[ -d "$DEFAULTS_DIR/bat_linux" ]]; then
            cp -r "$DEFAULTS_DIR/bat_linux" "$CURRENT_DIR/bat"
            echo "✅ batディレクトリをコピーしました"
        else
            echo "⚠️  $DEFAULTS_DIR/bat_linux が見つかりません"
        fi
    else
        echo "ℹ️  batディレクトリは既に存在します"
    fi
    
    # profileディレクトリのコピー
    if [[ ! -d "$CURRENT_DIR/profile" ]]; then
        if [[ -d "$DEFAULTS_DIR/profile" ]]; then
            cp -r "$DEFAULTS_DIR/profile" "$CURRENT_DIR/"
            echo "✅ profileディレクトリをコピーしました"
        else
            echo "⚠️  $DEFAULTS_DIR/profile が見つかりません"
        fi
    else
        echo "ℹ️  profileディレクトリは既に存在します"
    fi

    # drcsディレクトリのコピー
    if [[ ! -d "$CURRENT_DIR/drcs" ]]; then
        if [[ -d "$DEFAULTS_DIR/drcs" ]]; then
            cp -r "$DEFAULTS_DIR/drcs" "$CURRENT_DIR/"
            echo "✅ drcsディレクトリをコピーしました"
        else
            echo "⚠️  $DEFAULTS_DIR/drcs が見つかりません"
        fi
    else
        if [[ ! -f "$CURRENT_DIR/drcs/drcs_map.txt" ]]; then
            cp "$DEFAULTS_DIR/drcs/drcs_map.txt" "$CURRENT_DIR/drcs/drcs_map.txt"
            echo "✅ drcs_map.txtをコピーしました"
        else
            echo "ℹ️  drcsディレクトリは既に存在します"
        fi
    fi

    # JLディレクトリのコピー
    if [[ ! -d "$CURRENT_DIR/JL" ]]; then
        echo "JLディレクトリをダウンロード中..."
        (wget -q https://github.com/tobitti0/join_logo_scp/archive/refs/tags/Ver4.1.0_Linux.tar.gz -O JL.tar.gz \
            && tar -xf JL.tar.gz \
            && mv join_logo_scp-Ver4.1.0_Linux/JL "$CURRENT_DIR/" \
            && rm -rf join_logo_scp-Ver4.1.0_Linux/ join_logo_scp-Ver4.1.0_Linux JL.tar.gz \
            && echo "✅ JLディレクトリを作成しました" \
        )
    else
        echo "ℹ️  JLディレクトリは既に存在します"
    fi
    
    # 必要なディレクトリの作成
    echo ""
    echo "--- ディレクトリの作成 ---"
    
    local dirs=("config" "input" "logo" "output")
    for dir in "${dirs[@]}"; do
        if [[ ! -d "$CURRENT_DIR/$dir" ]]; then
            mkdir -p "$CURRENT_DIR/$dir"
            echo "✅ $dirディレクトリを作成しました"
        else
            echo "ℹ️  $dirディレクトリは既に存在します"
        fi
    done
    
    echo ""
    echo "--- compose.ymlファイルの準備 ---"
    
    # compose.ymlファイルの存在確認
    if [[ ! -f "compose.yml" && ! -f "docker-compose.yml" ]]; then
        if [[ -f "compose.sample.yml" ]]; then
            echo "⚠️  compose.ymlファイルが見つかりません。compose.sample.ymlをコピーします..."
            cp compose.sample.yml compose.yml
            echo "✅ compose.ymlファイルを作成しました"
        else
            echo "❌ compose.ymlファイルまたはcompose.sample.ymlが見つかりません"
            exit 1
        fi
    else
        echo "ℹ️  compose.ymlファイルは既に存在します"
    fi
    
    echo ""
    echo "================================"
    echo "✅ セットアップが完了しました！"
    echo ""
    echo "=== 次のステップ ==="
    echo "以下のコマンドを実行してコンテナを起動してください:"
    echo ""
    echo "  export RUN_UID=\$(id -u)"
    echo "  export RUN_GID=\$(id -g)"
    echo "  docker compose up -d"
    echo ""
    echo "=== その他のコマンド例 ==="
    echo "  ログ表示: docker compose logs -f"
    echo "  停止: docker compose down"
    echo "  再起動: docker compose restart"
    echo "  シェル実行: docker compose exec amatsukaze /bin/bash"
    echo ""
    echo "=== アクセス情報 ==="
    echo "Amatsukaze Server: http://localhost:32768"
    echo "================================"
}

# セットアップの実行
setup_directories
