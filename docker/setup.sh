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
    3. Compose設定ファイルの準備
    4. GPU環境に応じた推奨起動コマンドの表示

セットアップ完了後は以下のコマンドを実行してください:
    export RUN_UID=\$(id -u)
    export RUN_GID=\$(id -g)
    docker compose -f compose.yml up -d

例:
    # セットアップの実行
    $0

    # セットアップ完了後の起動
    export RUN_UID=\$(id -u)
    export RUN_GID=\$(id -g)
    docker compose -f compose.yml up -d
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

# GPU環境を検出して推奨起動コマンドを表示する
show_recommended_command() {
    local has_dri=0
    local has_nvidia=0
    local has_nvidia_runtime=0
    local compose_files=("$COMPOSE_BASE_FILE")

    echo ""
    echo "--- GPU環境の検出 ---"
    if [[ -n "${DOCKER_HOST:-}" ]]; then
        echo "⚠️  DOCKER_HOSTが設定されています。検出結果はこのマシン基準であり、接続先Dockerホストとは異なる場合があります"
    fi

    if [[ -e /dev/dri ]]; then
        has_dri=1
        echo "✅ /dev/dri を検出しました（QSV用overrideを利用できます）"
        compose_files+=("compose.qsv.yml")
    else
        echo "ℹ️  /dev/dri が見つかりません（QSV用overrideは追加しません）"
    fi

    if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi >/dev/null 2>&1; then
        has_nvidia=1
    elif [[ -d /proc/driver/nvidia ]]; then
        has_nvidia=1
    fi

    if [[ "$has_nvidia" -eq 1 ]]; then
        echo "✅ NVIDIA GPUを検出しました"
        if command -v docker >/dev/null 2>&1 \
            && docker info --format '{{range $name, $_ := .Runtimes}}{{println $name}}{{end}}' 2>/dev/null | grep -qx "nvidia"; then
            has_nvidia_runtime=1
            echo "✅ DockerのNVIDIA Runtimeを検出しました"
            compose_files+=("compose.nvidia.yml")
        else
            echo "⚠️  NVIDIA Container ToolkitがDocker Runtimeとして登録されていません"
            echo "   NVIDIA GPUを使うにはdocker/readme.mdのNVIDIA Container Toolkit導入手順を実行してください"
        fi
    else
        echo "ℹ️  NVIDIA GPUを検出できませんでした（NVIDIA用overrideは追加しません）"
    fi

    local command="docker compose"
    local compose_file
    for compose_file in "${compose_files[@]}"; do
        command+=" -f ${compose_file}"
    done
    command+=" up -d"
    RECOMMENDED_COMMAND="$command"

    echo ""
    echo "推奨起動コマンド:"
    echo "  $command"
    if [[ "$has_nvidia" -eq 1 && "$has_nvidia_runtime" -eq 0 ]]; then
        echo ""
        echo "NVIDIA Container Toolkit導入後は、上記コマンドに -f compose.nvidia.yml を追加してください"
    fi
}

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
    
    local dirs=("config" "data" "input" "logo" "output")
    for dir in "${dirs[@]}"; do
        if [[ ! -d "$CURRENT_DIR/$dir" ]]; then
            mkdir -p "$CURRENT_DIR/$dir"
            echo "✅ $dirディレクトリを作成しました"
        else
            echo "ℹ️  $dirディレクトリは既に存在します"
        fi
    done
    
    echo ""
    echo "--- Compose設定ファイルの準備 ---"
    
    # base Composeファイルの存在確認
    if [[ -f "$CURRENT_DIR/compose.yml" ]]; then
        COMPOSE_BASE_FILE="compose.yml"
        echo "ℹ️  compose.ymlファイルは既に存在します"
    elif [[ -f "$CURRENT_DIR/docker-compose.yml" ]]; then
        COMPOSE_BASE_FILE="docker-compose.yml"
        echo "ℹ️  docker-compose.ymlファイルは既に存在します"
    else
        if [[ -f "$SCRIPT_DIR/compose.sample.yml" ]]; then
            echo "⚠️  compose.ymlファイルが見つかりません。compose.sample.ymlをコピーします..."
            cp "$SCRIPT_DIR/compose.sample.yml" "$CURRENT_DIR/compose.yml"
            COMPOSE_BASE_FILE="compose.yml"
            echo "✅ compose.ymlファイルを作成しました"
        else
            echo "❌ compose.ymlファイルまたはcompose.sample.ymlが見つかりません"
            exit 1
        fi
    fi

    local override_sample
    local override_file
    for override_sample in "compose.qsv.sample.yml" "compose.nvidia.sample.yml"; do
        override_file="${override_sample/.sample/}"
        if [[ ! -f "$CURRENT_DIR/$override_file" ]]; then
            if [[ -f "$SCRIPT_DIR/$override_sample" ]]; then
                cp "$SCRIPT_DIR/$override_sample" "$CURRENT_DIR/$override_file"
                echo "✅ $override_fileを作成しました"
            else
                echo "❌ $override_sampleが見つかりません"
                exit 1
            fi
        else
            echo "ℹ️  $override_fileは既に存在します"
        fi
    done

    show_recommended_command
    
    echo ""
    echo "================================"
    echo "✅ セットアップが完了しました！"
    echo ""
    echo "=== 次のステップ ==="
    echo "$COMPOSE_BASE_FILE を編集して設定を行ってください"
    echo "- Amatsukazeを実行するユーザーIDとグループIDをRUN_UIDとRUN_GIDで指定"
    echo "- volumes のマウント対象等を調整"
    echo "- GPUを使う場合は、上に表示されたoverride付きの起動コマンドを使用"
    echo ""
    echo "その後以下のコマンドを実行してコンテナを起動してください:"
    echo ""
    echo "  $RECOMMENDED_COMMAND"
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
